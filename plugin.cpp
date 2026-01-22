#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include <cmath>
#include <unordered_set>
#include <chrono>
#include <SKSE/Logger.h>
#include <spdlog/sinks/basic_file_sink.h>

void InitializeLogging()
{
    auto path = SKSE::log::log_directory();
    if (!path) {
        return;
    }

    *path /= "Tracing.log";  // your plugin log file

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto logger = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}



namespace Tracing
{
    // Forward declarations
    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* vm);
    void PrintEquippedWeapon(RE::ActiveEffect* effect);
    void RegisterNearbyActor(RE::ActiveEffect* effect, RE::Actor* actor);
    void SummonMelee(RE::ActiveEffect* effect);
    void SummonRanged(RE::ActiveEffect* effect);
}

void OnSave(SKSE::SerializationInterface* ser);
void OnLoad(SKSE::SerializationInterface* ser);
void OnRevert(SKSE::SerializationInterface* ser);

// ─────────────────────────────────────────────────────────────
// Data Structures
// ─────────────────────────────────────────────────────────────

// Describes a weapon at the time it was observed.
// This is descriptive data only; it does not define identity.
struct WeaponSnapshot
{
    RE::FormID formID;
    RE::WEAPON_TYPE weaponType;
    float baseDamage;
    float enchantDamage;
    RE::FormID ammoFormID;      // Only relevant for ranged weapons
    float ammoDamage;           // Damage contribution from ammo
};


// Stores the descriptive data associated with a weapon combination.
// This data may grow over time and does not participate in identity.
struct DualWieldSnapshot
{
    WeaponSnapshot right;
    WeaponSnapshot left;
    RE::FormID shieldFormID = 0;    // Shield equipped in left hand (if any)
    float shieldArmorRating = 0.0f;    // Shield's armor rating for bias calculation
};

// Identifies a unique ordered weapon combination.
// This struct defines what "the same combination" means for lookup, hashing, and deduplication purposes.
struct WeaponCombinationKey
{
    RE::FormID rightFormID;
    RE::FormID leftFormID;

    bool operator==(const WeaponCombinationKey& other) const
    {
        return rightFormID == other.rightFormID &&
               leftFormID  == other.leftFormID;
    }
};

// Defines a hash function for WeaponCombinationKey to be used in unordered containers.
struct WeaponCombinationKeyHash
{
    std::size_t operator()(const WeaponCombinationKey& key) const noexcept
    {
        std::size_t h1 = std::hash<RE::FormID>{}(key.rightFormID);
        std::size_t h2 = std::hash<RE::FormID>{}(key.leftFormID);

        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

// Keeps track of summoned weapon(s)
struct PersistedSummonState
{
    RE::FormID right;
    RE::FormID left;
    RE::FormID ammo;    // For ranged weapons
    RE::FormID shield;  // Shield (if any)
};


// ─────────────────────────────────────────────────────────────
// Variable Initialization
// ─────────────────────────────────────────────────────────────

// Global registry mapping weapon combinations to their snapshots
using DualWieldRegistry = std::unordered_map<WeaponCombinationKey, DualWieldSnapshot, WeaponCombinationKeyHash>;
static DualWieldRegistry g_dualWieldRegistry;
constexpr std::uint32_t kDualWieldRecord = 'DWKR';

// FormID for no weapon (empty hand)
constexpr RE::FormID kEmptyHandFormID = 0;

// Keeps track of summoned weapons
static std::optional<PersistedSummonState> g_activeSummon;
constexpr std::uint32_t kSummonRecord = 'SUMN';

// Prevents summon spamming within a short interval
static std::chrono::steady_clock::time_point g_lastSummonTime;
constexpr auto kSummonCooldown = std::chrono::milliseconds(300);

// Prevents recursive despawn calls
static bool g_isDespawning = false;

// Periodic check for weapon state
static std::chrono::steady_clock::time_point g_lastStateCheck;
constexpr auto kStateCheckInterval = std::chrono::milliseconds(500);  // Check every 500ms

// Track recent summons to prevent repetition
static std::deque<WeaponCombinationKey> g_recentSummons;
constexpr std::size_t kRecentSummonsLimit = 5;

// Sheath tracker


// ─────────────────────────────────────────────────────────────
// Helper Functions
// ─────────────────────────────────────────────────────────────

// Print helper
inline const char* SafeName(const RE::TESForm* form, const char* fallback = "<no name>")
{
    if (!form) {
        return fallback;
    }
    const char* n = form->GetName();
    return (n && n[0]) ? n : fallback;
}

// Weapon "types"
inline bool IsEmpty(RE::FormID id)
{
    return id == kEmptyHandFormID;
}

inline bool IsTwoHanded(const WeaponCombinationKey& key)
{
    return key.rightFormID == key.leftFormID &&
           !IsEmpty(key.rightFormID);
}

inline bool IsSingleWeapon(const WeaponCombinationKey& key)
{
    return !IsEmpty(key.rightFormID) &&
           IsEmpty(key.leftFormID);
}

inline bool IsDualWield(const WeaponCombinationKey& key)
{
    return !IsEmpty(key.rightFormID) &&
           !IsEmpty(key.leftFormID) &&
           key.rightFormID != key.leftFormID;
}

// Melee vs Ranged Identification
inline bool IsRangedWeapon(RE::WEAPON_TYPE type)
{
    using WT = RE::WEAPON_TYPE;
    return type == WT::kBow || type == WT::kCrossbow;
}

inline bool IsMeleeWeapon(RE::WEAPON_TYPE type)
{
    return !IsRangedWeapon(type);
}

enum class CombatModality
{
    kMelee,
    kRanged,
    kMixed
};

inline CombatModality GetCombatModality(const DualWieldSnapshot& snapshot)
{
    bool rightRanged = IsRangedWeapon(snapshot.right.weaponType);
    bool leftRanged  = IsRangedWeapon(snapshot.left.weaponType);

    // Ranged weapon in right hand = ranged combat (left hand is always empty for ranged)
    if (rightRanged) {
        return CombatModality::kRanged;
    }
    // Both hands melee or empty = melee combat
    if (!rightRanged && !leftRanged) {
        return CombatModality::kMelee;
    }
    // One melee one ranged (shouldn't happen but handle it)
    return CombatModality::kMixed;
}

// Shield effectiveness calculations
inline float ShieldDamageEq(float armorRating)
{
    if (armorRating <= 0.0f) {
        return 0.0f;
    }

    constexpr float a = 1.1746001f;
    constexpr float b = 0.7149028f;
    return a * std::pow(armorRating, b);
}

inline float ShieldOffenseBias(float weaponDamage, float shieldArmorRating)
{
    float shieldEq = ShieldDamageEq(shieldArmorRating);

    constexpr float w = 1.0f;   // set to 0.5f if you want shield to count half as much
    float denom = weaponDamage + (w * shieldEq);

    if (denom <= 0.0f) {
        return 1.0f;
    }

    return weaponDamage / denom;
}

// ─────────────────────────────────────────────────────────────
// VFX Helper Functions
// ─────────────────────────────────────────────────────────────

void PlaySummonVFX(RE::Actor* actor)
{
    if (!actor) return;

    // VFX disabled - no reliable way to play bound weapon animation without issues
    SKSE::log::info("  VFX skipped");
}

void PlayDespawnVFX(RE::Actor* actor)
{
    if (!actor) return;

    // VFX disabled - same reason as summon VFX
    SKSE::log::info("  VFX skipped (requires Creation Kit ESP)");
}

// Enchantment damage computation
float ComputeEnchantmentDamage(RE::TESObjectWEAP* weapon)
{
    if (!weapon) {
        return 0.0f;
    }

    auto* enchantment = weapon->formEnchanting;
    if (!enchantment) {
        return 0.0f;
    }

    float total = 0.0f;

    for (auto& effect : enchantment->effects) {
        if (!effect) {
            continue;
        }

        auto* baseEffect = effect->baseEffect;
        if (!baseEffect) {
            continue;
        }

        // We only care about effects that directly deal damage
        using Archetype = RE::EffectSetting::Archetype;

        switch (baseEffect->GetArchetype()) {
        case Archetype::kValueModifier:
        case Archetype::kPeakValueModifier:
            // Damage Health, Absorb Health, etc.
            total += effect->effectItem.magnitude;
            break;

        default:
            break;
        }
    }

    return total;
}

// Weapon effectiveness
float ComputeCombinedDamage(const DualWieldSnapshot& snapshot)
{
    float right =
        snapshot.right.baseDamage +
        snapshot.right.enchantDamage +
        snapshot.right.ammoDamage;

    float left =
        snapshot.left.baseDamage +
        snapshot.left.enchantDamage +
        snapshot.left.ammoDamage;

    return right + left;
}

float ComputeBias(const WeaponCombinationKey& key, const DualWieldSnapshot& snapshot, float shieldArmorRating)
{
    float bias = 1.0f;

    // Two-handed weapons are baseline
    if (IsTwoHanded(key)) {
        return bias;
    }

    // Dual-wield normalization (melee only for now)
    if (IsDualWield(key)) {
        bias *= 0.70f;
    }

    // Shield influence applies only to melee
    CombatModality modality = GetCombatModality(snapshot);
    if (modality == CombatModality::kMelee && shieldArmorRating > 0.0f) {
        float weaponDamage = snapshot.right.baseDamage;
        bias *= ShieldOffenseBias(weaponDamage, shieldArmorRating);
    }

    return bias;
}

float EvaluateOffensivePower(const WeaponCombinationKey& key, const DualWieldSnapshot& snapshot, float shieldArmorRating)
{
    float baseDamage = ComputeCombinedDamage(snapshot);
    float bias       = ComputeBias(key, snapshot, shieldArmorRating);
    return baseDamage * bias;
}

// Builds a WeaponSnapshot
WeaponSnapshot BuildWeaponSnapshot(RE::TESObjectWEAP* weapon, RE::Actor* actor)
{
    WeaponSnapshot snapshot{};
    snapshot.formID        = weapon->GetFormID();
    snapshot.weaponType    = weapon->GetWeaponType();
    snapshot.baseDamage    = weapon->GetAttackDamage();
    snapshot.enchantDamage = ComputeEnchantmentDamage(weapon);
    snapshot.ammoFormID    = kEmptyHandFormID;
    snapshot.ammoDamage    = 0.0f;

    // If ranged weapon, try to get equipped ammo
    if (actor && IsRangedWeapon(snapshot.weaponType)) {
        auto* equippedAmmo = actor->GetCurrentAmmo();
        if (equippedAmmo) {
            snapshot.ammoFormID = equippedAmmo->GetFormID();
            snapshot.ammoDamage = equippedAmmo->data.damage;
            
            SKSE::log::info("    Ammo detected: {} (damage={})", 
                SafeName(equippedAmmo), snapshot.ammoDamage);
        }
    }

    return snapshot;
}

// Summoning
static std::vector<WeaponCombinationKey> g_meleeKeys;
static std::vector<WeaponCombinationKey> g_rangedKeys;

void RebuildCachedKeys()
{
    SKSE::log::info("Function start: RebuildCachedKeys");
    g_meleeKeys.clear();
    g_rangedKeys.clear();

    SKSE::log::info("  Cleared cached keys");

    for (const auto& [key, snapshot] : g_dualWieldRegistry) {
        CombatModality mod = GetCombatModality(snapshot);
        
        // Debug: log weapon type for each entry
        SKSE::log::info("  Entry {:X}/{:X}: right weaponType={}, left weaponType={}, modality={}",
            key.rightFormID, key.leftFormID,
            static_cast<int>(snapshot.right.weaponType),
            static_cast<int>(snapshot.left.weaponType),
            mod == CombatModality::kMelee ? "melee" : (mod == CombatModality::kRanged ? "ranged" : "mixed"));

        if (mod == CombatModality::kMelee) {
            g_meleeKeys.push_back(key);
        } else if (mod == CombatModality::kRanged) {
            g_rangedKeys.push_back(key);
        }
    }

    SKSE::log::info("  Finished rebuilding cached keys: melee={}, ranged={}",
        g_meleeKeys.size(), g_rangedKeys.size());
}

float ComputeSkillBias(float alteration)
{
    if (alteration <= 20.0f) {
        return 0.0f;
    }

    float x = (alteration - 20.0f) / 80.0f; // 1.0 at skill 100

    // Soft cap: approaches 1.0 asymptotically
    float bias = x / (0.25f + x);

    return std::clamp(bias, 0.0f, 1.0f);
}

float ComputeSelectionWeight(const WeaponCombinationKey& key, const DualWieldSnapshot& snapshot, float alteration, float shieldArmor)
{
    float power = EvaluateOffensivePower(key, snapshot, shieldArmor);

    // Defensive clamp
    power = std::max(power, 1.0f);

    float bias = ComputeSkillBias(alteration);

    // Bias controls exponent strength
    constexpr float kMaxExponent = 3.0f;

    float exponent = 1.0f + bias * kMaxExponent;

    return std::pow(power, exponent);
}

WeaponCombinationKey PickWeighted(const std::vector<WeaponCombinationKey>& keys)
{
    SKSE::log::info("Function start: PickWeighted");

    // Filter out recently used weapons (last 5 summons)
    std::vector<WeaponCombinationKey> availableKeys;
    for (const auto& key : keys) {
        bool recentlyUsed = false;
        for (const auto& recent : g_recentSummons) {
            if (recent.rightFormID == key.rightFormID && recent.leftFormID == key.leftFormID) {
                recentlyUsed = true;
                break;
            }
        }
        if (!recentlyUsed) {
            availableKeys.push_back(key);
        }
    }
    
    // If all weapons were recently used, allow all of them
    if (availableKeys.empty()) {
        SKSE::log::info("  All weapons recently used, resetting available pool");
        availableKeys = keys;
    } else {
        SKSE::log::info("  Filtered {} candidates (excluded {} recently used)", availableKeys.size(), keys.size() - availableKeys.size());
    }

    float alteration = 0.0f;
    if (alteration == 0.0f) {
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            alteration = player->AsActorValueOwner()->GetActorValue(RE::ActorValue ::kAlteration);
            SKSE::log::info("  Alteration from player fallback = {}", alteration);
        }
    }

    float shieldArmor = 0.0f; // optional later

    std::vector<float> weights;
    weights.reserve(availableKeys.size());

    float total = 0.0f;

    SKSE::log::info("  Beginning weight computations for {} candidates", availableKeys.size());

    for (const auto& key : availableKeys) {
        const auto& snap = g_dualWieldRegistry.at(key);
        float w = ComputeSelectionWeight(
            key, snap, alteration, shieldArmor);

        weights.push_back(w);
        total += w;
    }

    SKSE::log::info("  Completed weight computations; total weight={}", total);

    std::uniform_real_distribution<float> dist(0.0f, total);
    static std::mt19937 rng{ std::random_device{}() };

    float r = dist(rng);

    SKSE::log::info("  Choosing candidate with random threshold: {}", r);
    for (std::size_t i = 0; i < availableKeys.size(); ++i) {
        if ((r -= weights[i]) <= 0.0f) {
            WeaponCombinationKey selected = availableKeys[i];
            
            // Track this summon in recent history
            g_recentSummons.push_back(selected);
            if (g_recentSummons.size() > kRecentSummonsLimit) {
                g_recentSummons.pop_front();
            }
            
            return selected;
        }
    }

    // Defensive fallback - also track it
    WeaponCombinationKey fallback = availableKeys.back();
    g_recentSummons.push_back(fallback);
    if (g_recentSummons.size() > kRecentSummonsLimit) {
        g_recentSummons.pop_front();
    }
    return fallback;
}

void UnequipWeapons(RE::Actor* actor)
{
    if (!actor) {
        return;
    }

    auto* right = actor->GetEquippedObject(false);
    auto* left  = actor->GetEquippedObject(true);

    if (right) {
        actor->UnequipItem(0, static_cast<RE::TESBoundObject*>(right));
    }

    if (left && left != right) {
        actor->UnequipItem(0, static_cast<RE::TESBoundObject*>(left));
    }
}

void ForceDespawnSummon(RE::Actor* actor)
{
    SKSE::log::info("Function start: ForceDespawnSummon");

    // Prevent recursive calls
    if (g_isDespawning) {
        SKSE::log::info("  Already despawning; return");
        return;
    }

    SKSE::log::info("  Checking active summon");
    if (!g_activeSummon) {
        SKSE::log::info("  No active summon to despawn; return");
        SKSE::log::info("Function end: ForceDespawnSummon");
        return;
    }

    g_isDespawning = true;

    // ─────────────────────────────────────────────
    // Play despawn VFX
    // ─────────────────────────────────────────────
    PlayDespawnVFX(actor);

    // Unequip all weapons once before removal
    SKSE::log::info("  Unequipping weapons");
    UnequipWeapons(actor);

    SKSE::log::info("  Despawning weapons");
    auto remove = [&](RE::FormID id)
    {
        if (!id || id == kEmptyHandFormID) return;

        auto* form = RE::TESForm::LookupByID(id);
        auto* weap = form ? form->As<RE::TESObjectWEAP>() : nullptr;
        if (!weap) return;

        actor->RemoveItem(
            weap,
            1,
            RE::ITEM_REMOVE_REASON::kRemove,
            nullptr,
            nullptr
        );
    };

    remove(g_activeSummon->right);
    remove(g_activeSummon->left);

    // Remove ammo if present
    if (g_activeSummon->ammo && g_activeSummon->ammo != kEmptyHandFormID) {
        SKSE::log::info("  Despawning ammo");
        auto* ammoForm = RE::TESForm::LookupByID(g_activeSummon->ammo);
        auto* ammo = ammoForm ? ammoForm->As<RE::TESAmmo>() : nullptr;
        if (ammo) {
            // Remove a large number to ensure all summoned arrows are gone
            // (999 should cover all cases without needing to query inventory)
            SKSE::log::info("    Removing summoned arrows");
            actor->RemoveItem(
                ammo,
                999,
                RE::ITEM_REMOVE_REASON::kRemove,
                nullptr,
                nullptr
            );
        }
    }

    // Remove shield if present
    if (g_activeSummon->shield && g_activeSummon->shield != kEmptyHandFormID) {
        SKSE::log::info("  Despawning shield");
        auto* shieldForm = RE::TESForm::LookupByID(g_activeSummon->shield);
        auto* shield = shieldForm ? shieldForm->As<RE::TESObjectARMO>() : nullptr;
        if (shield) {
            SKSE::log::info("    Removing summoned shield");
            actor->RemoveItem(
                shield,
                1,
                RE::ITEM_REMOVE_REASON::kRemove,
                nullptr,
                nullptr
            );
        }
    }

    SKSE::log::info("  Cleared active summon state in g_activeSummon");
    g_activeSummon.reset();

    g_isDespawning = false;

    SKSE::log::info("Function end: ForceDespawnSummon");
}

void SummonWeaponCombination(RE::Actor* player, const WeaponCombinationKey& key)
{
    SKSE::log::info("Function start: SummonWeaponCombination");

    if (!player) {
        SKSE::log::critical("  Player actor is null; aborting summon");
        return;
    }

    // ─────────────────────────────────────────────
    // Despawn previous summon
    // ─────────────────────────────────────────────
    SKSE::log::info("  Despawning existing summon");
    ForceDespawnSummon(player);

    // ─────────────────────────────────────────────
    // Resolve right weapon (try traced version first)
    // ─────────────────────────────────────────────
    SKSE::log::info("  Looking up right weapon {:X}", key.rightFormID);

    auto* rightForm = RE::TESForm::LookupByID(key.rightFormID);
    auto* rightWeapon = rightForm ? rightForm->As<RE::TESObjectWEAP>() : nullptr;

    if (!rightWeapon) {
        SKSE::log::critical("  Right weapon lookup FAILED");
        return;
    }

    // Try to find traced (bound) version by name
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    RE::TESObjectWEAP* rightWeaponToUse = rightWeapon;
    
    if (dataHandler) {
        std::string originalName = rightWeapon->GetName();
        std::string tracedName = originalName + " (Traced)";
        SKSE::log::info("  Looking for traced version with name: {}", tracedName);
        
        // Search through all weapons for the traced version WITH bound flag
        auto& weapons = dataHandler->GetFormArray<RE::TESObjectWEAP>();
        for (auto* weapon : weapons) {
            if (weapon && weapon->GetName()) {
                if (std::string(weapon->GetName()) == tracedName) {
                    // Check if this version has the bound weapon flag
                    if (weapon->IsBound()) {
                        SKSE::log::info("  Found traced version: {}", SafeName(weapon));
                        rightWeaponToUse = weapon;
                        break;
                    } else {
                        SKSE::log::info("  Found weapon with matching name but NO bound flag: {}", SafeName(weapon));
                    }
                }
            }
        }
        
        if (rightWeaponToUse == rightWeapon) {
            SKSE::log::info("  No traced version found, using original: {}", SafeName(rightWeapon));
        }
    }

    SKSE::log::info("  Right weapon OK: {}", SafeName(rightWeaponToUse));
    
    // Log weapon details for debugging
    SKSE::log::info("  Weapon FormID: {:X}", rightWeaponToUse->GetFormID());
    SKSE::log::info("  Weapon Editor ID: {}", rightWeaponToUse->GetFormEditorID() ? rightWeaponToUse->GetFormEditorID() : "<none>");
    
    if (rightWeaponToUse->GetWeaponType() == RE::WEAPON_TYPE::kBow) {
        SKSE::log::info("  Weapon is a BOW");
    }
    
    // Debug: Check both flags fields
    SKSE::log::info("  WeaponData.flags raw value: {:X}", rightWeaponToUse->weaponData.flags.underlying());
    SKSE::log::info("  WeaponData.flags2 raw value: {:X}", rightWeaponToUse->weaponData.flags2.underlying());
    
    // Check bound weapon flag using built-in method
    bool hasBoundFlag = rightWeaponToUse->IsBound();
    SKSE::log::info("  IsBound() returns: {}", hasBoundFlag ? "true" : "false");
    
    // Check manually in flags2
    bool hasBoundFlag2 = rightWeaponToUse->weaponData.flags2.any(RE::TESObjectWEAP::Data::Flag2::kBoundWeapon);
    SKSE::log::info("  Manual flags2 check returns: {}", hasBoundFlag2 ? "true" : "false");
    
    if (!hasBoundFlag && !hasBoundFlag2) {
        SKSE::log::info("  WARNING: Weapon does NOT have bound weapon flag!");
        SKSE::log::info("  This means TracedWeaponDuplicator.esp is not loaded or the traced weapon wasn't found");
        SKSE::log::info("  The animation will NOT play without the bound weapon flag");
    }

    // ─────────────────────────────────────────────
    // Resolve left weapon (try traced version first)
    // ─────────────────────────────────────────────
    RE::TESObjectWEAP* leftWeaponToUse = nullptr;

    if (!IsEmpty(key.leftFormID)) {
        SKSE::log::info("  Looking up left weapon {:X}", key.leftFormID);

        auto* leftForm = RE::TESForm::LookupByID(key.leftFormID);
        auto* leftWeapon = leftForm ? leftForm->As<RE::TESObjectWEAP>() : nullptr;

        if (!leftWeapon) {
            SKSE::log::warn("  Left weapon lookup FAILED");
        } else {
            leftWeaponToUse = leftWeapon;
            
            // Try to find traced version by name
            if (dataHandler) {
                std::string originalName = leftWeapon->GetName();
                std::string tracedName = originalName + " (Traced)";
                SKSE::log::info("  Looking for traced version with name: {}", tracedName);
                
                // Search through all weapons for the traced version WITH bound flag
                auto& weapons = dataHandler->GetFormArray<RE::TESObjectWEAP>();
                for (auto* weapon : weapons) {
                    if (weapon && weapon->GetName()) {
                        if (std::string(weapon->GetName()) == tracedName) {
                            // Check if this version has the bound weapon flag
                            if (weapon->IsBound()) {
                                SKSE::log::info("  Found traced version: {}", SafeName(weapon));
                                leftWeaponToUse = weapon;
                                break;
                            } else {
                                SKSE::log::info("  Found weapon with matching name but NO bound flag: {}", SafeName(weapon));
                            }
                        }
                    }
                }
                
                if (leftWeaponToUse == leftWeapon) {
                    SKSE::log::info("  No traced version found, using original: {}", SafeName(leftWeapon));
                }
            }
            
            SKSE::log::info("  Left weapon OK: {}", SafeName(leftWeaponToUse));
        }
    }

    // ─────────────────────────────────────────────
    // Resolve and add ammo for ranged weapons (try traced version first)
    // ─────────────────────────────────────────────
    const auto& snapshot = g_dualWieldRegistry.at(key);
    RE::TESAmmo* ammo = nullptr;

    if (!IsEmpty(snapshot.right.ammoFormID)) {
        SKSE::log::info("  Looking up ammo {:X}", snapshot.right.ammoFormID);

        auto* ammoForm = RE::TESForm::LookupByID(snapshot.right.ammoFormID);
        auto* originalAmmo = ammoForm ? ammoForm->As<RE::TESAmmo>() : nullptr;

        if (!originalAmmo) {
            SKSE::log::warn("  Ammo lookup FAILED");
        } else {
            ammo = originalAmmo;
            
            // Try to find traced version by name
            if (dataHandler) {
                std::string originalName = originalAmmo->GetName();
                std::string tracedName = originalName + " (Traced)";
                SKSE::log::info("  Looking for traced ammo with name: {}", tracedName);

                // Search through all ammo for the traced version
                auto& ammos = dataHandler->GetFormArray<RE::TESAmmo>();
                for (auto* testAmmo : ammos) {
                    if (testAmmo && testAmmo->GetName()) {
                        if (std::string(testAmmo->GetName()) == tracedName) {
                            SKSE::log::info("  Found traced ammo: {}", SafeName(testAmmo));
                            ammo = testAmmo;
                            break;
                        }
                    }
                }

                if (ammo == originalAmmo) {
                    SKSE::log::info("  No traced ammo found, using original: {}", SafeName(originalAmmo));
                }
            }
            
            SKSE::log::info("  Ammo OK: {}", SafeName(ammo));
        }
    }

    // ─────────────────────────────────────────────
    // Resolve and add shield (try traced version first)
    // ─────────────────────────────────────────────
    RE::TESObjectARMO* shieldToUse = nullptr;

    if (!IsEmpty(snapshot.shieldFormID)) {
        SKSE::log::info("  Looking up shield {:X}", snapshot.shieldFormID);

        auto* shieldForm = RE::TESForm::LookupByID(snapshot.shieldFormID);
        auto* shield = shieldForm ? shieldForm->As<RE::TESObjectARMO>() : nullptr;

        if (!shield) {
            SKSE::log::warn("  Shield lookup FAILED");
        } else {
            // Try to find traced version by name
            if (dataHandler) {
                std::string originalName = shield->GetName();
                std::string tracedName = originalName + " (Traced)";
                SKSE::log::info("  Looking for traced shield with name: {}", tracedName);

                // Search through all armor for the traced version
                auto& armors = dataHandler->GetFormArray<RE::TESObjectARMO>();
                for (auto* armor : armors) {
                    if (armor && armor->GetName()) {
                        if (std::string(armor->GetName()) == tracedName) {
                            // Verify it's actually a shield (not other armor)
                            if (armor->IsShield()) {
                                SKSE::log::info("  Found traced shield: {}", SafeName(armor));
                                shieldToUse = armor;
                                break;
                            }
                        }
                    }
                }

                if (!shieldToUse) {
                    SKSE::log::info("  No traced shield found, using original: {}", SafeName(shield));
                    shieldToUse = shield;
                }
            } else {
                shieldToUse = shield;
            }
        }
    }

    // ─────────────────────────────────────────────
    // ADD TO INVENTORY
    // ─────────────────────────────────────────────
    SKSE::log::info("  Adding right weapon to inventory");
    player->AddObjectToContainer(rightWeaponToUse, nullptr, 1, nullptr);

    if (leftWeaponToUse) {
        SKSE::log::info("  Adding left weapon to inventory");
        player->AddObjectToContainer(leftWeaponToUse, nullptr, 1, nullptr);
    }

    if (ammo) {
        SKSE::log::info("  Adding ammo to inventory");
        player->AddObjectToContainer(ammo, nullptr, 100, nullptr);
    }

    if (shieldToUse) {
        SKSE::log::info("  Adding shield to inventory");
        player->AddObjectToContainer(shieldToUse, nullptr, 1, nullptr);
    }

    // ─────────────────────────────────────────────
    // Play summon VFX
    // ─────────────────────────────────────────────
    PlaySummonVFX(player);

    // ─────────────────────────────────────────────
    // Equip weapons immediately (no delay for bound weapon animation)
    // ─────────────────────────────────────────────
    SKSE::log::info("  Equipping weapons");
    
    auto* equipMgr = RE::ActorEquipManager::GetSingleton();
    if (!equipMgr) {
        SKSE::log::critical("  EquipManager is null");
        return;
    }

    // Equip right weapon
    if (rightWeaponToUse) {
        SKSE::log::info("  Equipping right weapon");
        
        // Bows and crossbows equip to LEFT hand slot
        bool isRanged = (rightWeaponToUse->GetWeaponType() == RE::WEAPON_TYPE::kBow || 
                        rightWeaponToUse->GetWeaponType() == RE::WEAPON_TYPE::kCrossbow);
        
        if (isRanged) {
            SKSE::log::info("  Ranged weapon - using default equip (left hand)");
        }
        
        // Use default equip - let the game handle slot assignment
        equipMgr->EquipObject(player, rightWeaponToUse);
    }

    // Equip left weapon if present
    if (leftWeaponToUse) {
        SKSE::log::info("  Equipping left weapon");
        equipMgr->EquipObject(player, leftWeaponToUse);
    }

    // Equip shield if present
    if (shieldToUse) {
        SKSE::log::info("  Equipping shield");
        equipMgr->EquipObject(player, shieldToUse);
    }

    // Equip ammo if present
    if (ammo) {
        SKSE::log::info("  Equipping ammo");
        equipMgr->EquipObject(player, ammo);
    }

    SKSE::log::info("  Equip complete");

    // ─────────────────────────────────────────────
    // Record summon state
    // ─────────────────────────────────────────────
    g_activeSummon = { 
        rightWeaponToUse->GetFormID(), 
        leftWeaponToUse ? leftWeaponToUse->GetFormID() : kEmptyHandFormID, 
        ammo ? ammo->GetFormID() : kEmptyHandFormID,
        shieldToUse ? shieldToUse->GetFormID() : kEmptyHandFormID
    };
    SKSE::log::info("  g_activeSummon updated");

    SKSE::log::info("Function end: SummonWeaponCombination");
}

bool CanSummon()
{
    auto now = std::chrono::steady_clock::now();
    if (now - g_lastSummonTime < kSummonCooldown) {
        return false;
    }
    g_lastSummonTime = now;
    return true;
}

void PeriodicStateCheck()
{
    auto now = std::chrono::steady_clock::now();
    if (now - g_lastStateCheck < kStateCheckInterval) {
        return;
    }
    g_lastStateCheck = now;

    if (!g_activeSummon) {
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }

    // Check 1: Is player critically injured?
    float currentHealth = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
    float maxHealth = player->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kHealth);
    
    if (maxHealth > 0.0f) {
        float healthPercent = (currentHealth / maxHealth) * 100.0f;
        if (healthPercent <= 5.0f || currentHealth <= 1.0f) {
            SKSE::log::info("[StateCheck] Critical health detected ({}%), despawning", healthPercent);
            ForceDespawnSummon(player);
            return;
        }
    }

    // Check 2: Are weapons still equipped?
    auto* rightEquipped = player->GetEquippedObject(false);
    auto* leftEquipped = player->GetEquippedObject(true);

    bool rightMatch = false;
    bool leftMatch = false;

    if (rightEquipped) {
        rightMatch = (rightEquipped->GetFormID() == g_activeSummon->right);
    }
    
    if (leftEquipped) {
        leftMatch = (leftEquipped->GetFormID() == g_activeSummon->left);
    }

    // If neither summoned weapon is equipped, despawn
    if (!rightMatch && !leftMatch) {
        SKSE::log::info("[StateCheck] Summoned weapons not equipped, despawning");
        ForceDespawnSummon(player);
        return;
    }
}

void CastSummonSpell(CombatModality mode)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        SKSE::log::critical("CastSummonSpell: player null");
        return;
    }

    if (!CanSummon()) {
        SKSE::log::info("CastSummonSpell: blocked duplicate cast");
        return;
    }

    // Ensure pools exist
    if (g_meleeKeys.empty() && g_rangedKeys.empty() && !g_dualWieldRegistry.empty()) {
        RebuildCachedKeys();
    }

    const auto& pool = (mode == CombatModality::kMelee) ? g_meleeKeys : g_rangedKeys;

    if (pool.empty()) {
        const char* modeName = (mode == CombatModality::kMelee) ? "melee" : "ranged";
        SKSE::log::warn("CastSummonSpell: no candidates for mode={}", modeName);
        
        // Notify player via console message
        RE::DebugNotification("No suitable weapons found. Observe NPCs with the weapon type you want to trace.");
        return;
    }

    WeaponCombinationKey chosen = PickWeighted(pool);

    SKSE::log::info(
        "CastSummonSpell: picked right={:X} left={:X} (pool={})",
        chosen.rightFormID, chosen.leftFormID, pool.size()
    );

    SummonWeaponCombination(player, chosen);
}

// Unsummoning
class PlayerAnimationListener final :
    public RE::BSTEventSink<RE::BSAnimationGraphEvent>
{
public:
    static PlayerAnimationListener* GetSingleton()
    {
        static PlayerAnimationListener instance;
        return std::addressof(instance);
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::BSAnimationGraphEvent* event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
    {
        if (!event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // SKSE::log::info("[PlayerAnimListener] tag='{}'", event->tag.c_str());

        if (event->tag == "WeaponSheathe") {
            SKSE::log::info("[PlayerAnimListener] WeaponSheathe detected");

            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                SKSE::log::info("[PlayerAnimListener] Despawning summoned weapon");
                ForceDespawnSummon(player);
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};

class PlayerEquipListener final :
    public RE::BSTEventSink<RE::TESEquipEvent>
{
public:
    static PlayerEquipListener* GetSingleton()
    {
        static PlayerEquipListener instance;
        return std::addressof(instance);
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESEquipEvent* event,
        RE::BSTEventSource<RE::TESEquipEvent>*) override
    {
        if (!event || !g_activeSummon) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || event->actor.get() != player) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Check if player is unequipping a summoned weapon
        if (!event->equipped && 
            (event->baseObject == g_activeSummon->right || 
             event->baseObject == g_activeSummon->left)) {
            
            SKSE::log::info("[EquipListener] Player unequipped summoned weapon; despawning");
            ForceDespawnSummon(player);
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};

class PlayerInventoryListener final :
    public RE::BSTEventSink<RE::TESContainerChangedEvent>
{
public:
    static PlayerInventoryListener* GetSingleton()
    {
        static PlayerInventoryListener instance;
        return std::addressof(instance);
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESContainerChangedEvent* event,
        RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
    {
        if (!event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || event->newContainer != player->GetFormID()) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Only track items being added (not removed)
        if (event->itemCount <= 0) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Check if the item is a weapon
        auto* form = RE::TESForm::LookupByID(event->baseObj);
        auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
        
        if (!weapon) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Build a key for this weapon (right hand only, no left)
        RE::FormID weaponFormID = weapon->GetFormID();
        WeaponCombinationKey key{ weaponFormID, kEmptyHandFormID };

        // Check if already in registry
        if (g_dualWieldRegistry.contains(key)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        SKSE::log::info("[InventoryListener] Weapon added to inventory: {}", SafeName(weapon));

        // Build snapshot for this weapon
        DualWieldSnapshot snapshot{};
        snapshot.right = BuildWeaponSnapshot(weapon, player);
        
        // Empty left hand
        snapshot.left = WeaponSnapshot{};
        snapshot.left.formID = kEmptyHandFormID;
        snapshot.left.weaponType = RE::WEAPON_TYPE::kHandToHandMelee;
        snapshot.left.baseDamage = 0.0f;
        snapshot.left.enchantDamage = 0.0f;
        snapshot.left.ammoFormID = kEmptyHandFormID;
        snapshot.left.ammoDamage = 0.0f;

        // No shield
        snapshot.shieldFormID = kEmptyHandFormID;
        snapshot.shieldArmorRating = 0.0f;

        // Add to registry
        g_dualWieldRegistry.emplace(key, snapshot);
        SKSE::log::info("  Weapon added to registry from inventory");
        
        // Rebuild cached keys
        RebuildCachedKeys();

        return RE::BSEventNotifyControl::kContinue;
    }

private:
    PlayerInventoryListener() = default;
    PlayerInventoryListener(const PlayerInventoryListener&) = delete;
    PlayerInventoryListener(PlayerInventoryListener&&) = delete;
    ~PlayerInventoryListener() = default;

    PlayerInventoryListener& operator=(const PlayerInventoryListener&) = delete;
    PlayerInventoryListener& operator=(PlayerInventoryListener&&) = delete;
};

class PlayerHealthListener final :
    public RE::BSTEventSink<RE::TESHitEvent>
{
public:
    static PlayerHealthListener* GetSingleton()
    {
        static PlayerHealthListener instance;
        return std::addressof(instance);
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESHitEvent* event,
        RE::BSTEventSource<RE::TESHitEvent>*) override
    {
        if (!event || !g_activeSummon) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || event->target.get() != player) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Check if health is critically low (for death alternative mods)
        float currentHealth = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
        float maxHealth = player->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kHealth);

        if (maxHealth <= 0.0f) {
            return RE::BSEventNotifyControl::kContinue;
        }

        float healthPercent = (currentHealth / maxHealth) * 100.0f;

        // If health drops to 5% or below, auto-despawn summoned weapons
        if (healthPercent <= 5.0f || currentHealth <= 1.0f) {
            SKSE::log::info("[HealthListener] Player health critical ({}%), despawning weapons", healthPercent);
            ForceDespawnSummon(player);
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};


// ─────────────────────────────────────────────────────────────
// Save/Load/Revert Handlers
// ─────────────────────────────────────────────────────────────

void RegisterSerialization()
{
    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID('UBWT');  // must be unique to your plugin

    serialization->SetSaveCallback(OnSave);
    serialization->SetLoadCallback(OnLoad);
    serialization->SetRevertCallback(OnRevert);
}

void OnSave(SKSE::SerializationInterface* ser)
{
    if (g_activeSummon) {
        if (!ser->OpenRecord(kSummonRecord, 1)) {
            return;
        }

        ser->WriteRecordData(&g_activeSummon->right, sizeof(RE::FormID));
        ser->WriteRecordData(&g_activeSummon->left, sizeof(RE::FormID));
        ser->WriteRecordData(&g_activeSummon->ammo, sizeof(RE::FormID));
    }

    if (!ser->OpenRecord(kDualWieldRecord, 1)) {
        SKSE::log::warn("OnSave: failed to open serialization record");
        return;
    }

    std::uint32_t count =
        static_cast<std::uint32_t>(g_dualWieldRegistry.size());

    ser->WriteRecordData(&count, sizeof(count));

    for (const auto& [key, snapshot] : g_dualWieldRegistry) {
        ser->WriteRecordData(&key, sizeof(key));
        ser->WriteRecordData(&snapshot, sizeof(snapshot));
    }

    SKSE::log::info("Saved weapon registry");
}

void OnLoad(SKSE::SerializationInterface* ser)
{
    std::uint32_t type;
    std::uint32_t version;
    std::uint32_t length;

    // Important: do NOT clear here unless you actually read a registry record.
    while (ser->GetNextRecordInfo(type, version, length)) {

        if (type == kSummonRecord) {
            PersistedSummonState state{};
            ser->ReadRecordData(&state.right, sizeof(RE::FormID));
            ser->ReadRecordData(&state.left,  sizeof(RE::FormID));
            ser->ReadRecordData(&state.ammo,  sizeof(RE::FormID));
            g_activeSummon = state;
            continue;
        }

        if (type == kDualWieldRecord) {
            g_dualWieldRegistry.clear();

            std::uint32_t count = 0;
            ser->ReadRecordData(&count, sizeof(count));

            for (std::uint32_t i = 0; i < count; ++i) {
                WeaponCombinationKey key{};
                DualWieldSnapshot snapshot{};

                ser->ReadRecordData(&key, sizeof(key));
                ser->ReadRecordData(&snapshot, sizeof(snapshot));

                // Validate FormIDs to catch corrupted save data
                bool validKey = true;
                
                // Check if right FormID looks valid (not 0, not obviously corrupted)
                if (key.rightFormID == 0 || key.rightFormID > 0x0FFFFFFF) {
                    SKSE::log::warn("  Skipping entry with invalid right FormID: {:X}", key.rightFormID);
                    validKey = false;
                }
                
                // Check if left FormID is either empty or valid
                if (key.leftFormID != 0 && key.leftFormID != kEmptyHandFormID && key.leftFormID > 0x0FFFFFFF) {
                    SKSE::log::warn("  Skipping entry with invalid left FormID: {:X}", key.leftFormID);
                    validKey = false;
                }
                
                // Check if shield FormID is either empty or looks valid
                if (snapshot.shieldFormID != 0 && snapshot.shieldFormID > 0x0FFFFFFF) {
                    SKSE::log::warn("  Resetting invalid shield FormID: {:X}", snapshot.shieldFormID);
                    snapshot.shieldFormID = 0;
                    snapshot.shieldArmorRating = 0.0f;
                }
                
                if (validKey) {
                    g_dualWieldRegistry.emplace(key, snapshot);
                }
            }

            continue;
        }

        // Unknown record: consume payload bytes
        std::vector<std::uint8_t> discard(length);
        ser->ReadRecordData(discard.data(), length);
    }

    RebuildCachedKeys();

    SKSE::log::info(
        "Loaded: registry={} meleeKeys={} rangedKeys={} activeSummon={}",
        g_dualWieldRegistry.size(),
        g_meleeKeys.size(),
        g_rangedKeys.size(),
        g_activeSummon ? "yes" : "no"
    );
}


void OnRevert(SKSE::SerializationInterface*)
{
    g_activeSummon.reset();
    g_dualWieldRegistry.clear();
}


// ─────────────────────────────────────────────────────────────
// Papyrus registration
// ─────────────────────────────────────────────────────────────
namespace Tracing
{
    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* vm)
    {
        vm->RegisterFunction(
            "PrintEquippedWeapon",
            "TracingTestEffect",   // MUST match the Papyrus script name
            PrintEquippedWeapon
        );

        vm->RegisterFunction(
            "RegisterNearbyActor",
            "TracingTargetEffect",
            RegisterNearbyActor
        );

        vm->RegisterFunction(
            "SummonMelee",
            "TracingSummonEffect",
            SummonMelee
        );

        vm->RegisterFunction(
            "SummonRanged",
            "TracingSummonEffect",
            SummonRanged
        );
        
        return true;
    }
    void TraceActorLoadout(RE::Actor* actor)    
    {
        SKSE::log::info("TraceActorLoadout: START");

        if (!actor) {
            SKSE::log::warn("TraceActorLoadout: actor is null");
            return;
        }

        auto formID = actor->GetFormID();
        SKSE::log::info("  FormID: {:X}", formID);

        // Query equipped weapons
        auto* rightHand = actor->GetEquippedObject(false);
        auto* leftHand = actor->GetEquippedObject(true);

        auto* rightWeapon = rightHand ? rightHand->As<RE::TESObjectWEAP>() : nullptr;
        auto* leftWeapon = leftHand ? leftHand->As<RE::TESObjectWEAP>() : nullptr;
        auto* shield = leftHand ? leftHand->As<RE::TESObjectARMO>() : nullptr;

        SKSE::log::info("  Right weapon: {}", SafeName(rightWeapon, "<empty>"));
        SKSE::log::info("  Left weapon: {}", SafeName(leftWeapon, "<empty>"));
        SKSE::log::info("  Shield: {}", SafeName(shield, "<empty>"));

        // Reject combinations with no right-hand weapon
        if (!rightWeapon) {
            SKSE::log::info(
                "  Weapon combination NOT stored: no right-hand weapon equipped");
            SKSE::log::info("TraceActorLoadout: END");
            return;
        }

        // Build weapon combination key
        RE::FormID rightFormID = rightWeapon->GetFormID();
        RE::FormID leftFormID  = leftWeapon ? leftWeapon->GetFormID() : kEmptyHandFormID;

        // Ranged weapons cannot be dual-wielded; force left hand to empty
        RE::WEAPON_TYPE rightType = rightWeapon->GetWeaponType();
        if (IsRangedWeapon(rightType)) {
            leftFormID = kEmptyHandFormID;
        }

        // Two-handed weapons occupy both hands but should only be stored once
        if (rightFormID == leftFormID) {
            leftFormID = kEmptyHandFormID;  // Prevent duplicate storage
        }

        SKSE::log::info("  Right FormID: {:X}", rightFormID);
        SKSE::log::info("  Left FormID: {:X}", leftFormID);

        WeaponCombinationKey key{ rightFormID, leftFormID };

        // Check if already registered
        if (g_dualWieldRegistry.contains(key)) {
            SKSE::log::info(
                "  Weapon combination already exists in registry");
            SKSE::log::info("TraceActorLoadout: END");
            return;
        }

        // Build and store snapshot
        DualWieldSnapshot snapshot{};
        snapshot.right = BuildWeaponSnapshot(rightWeapon, actor);
        SKSE::log::info("    Right weapon damage: {}", snapshot.right.baseDamage);

        // Only build left snapshot if left hand is actually used (after ranged/two-handed checks)
        if (!IsEmpty(leftFormID) && leftWeapon) {
            snapshot.left = BuildWeaponSnapshot(leftWeapon, actor);
            SKSE::log::info("    Left weapon damage: {}", snapshot.left.baseDamage);
        } else {
            // Initialize empty left hand snapshot
            snapshot.left = WeaponSnapshot{};
            snapshot.left.formID = kEmptyHandFormID;
            snapshot.left.weaponType = RE::WEAPON_TYPE::kHandToHandMelee;  // Empty hand
            snapshot.left.baseDamage = 0.0f;
            snapshot.left.enchantDamage = 0.0f;
            snapshot.left.ammoFormID = kEmptyHandFormID;
            snapshot.left.ammoDamage = 0.0f;
        }

        // Store shield information if present
        if (shield) {
            snapshot.shieldFormID = shield->GetFormID();
            snapshot.shieldArmorRating = shield->GetArmorRating();
            SKSE::log::info("    Shield: {} (rating={})", SafeName(shield), snapshot.shieldArmorRating);
        } else {
            snapshot.shieldFormID = kEmptyHandFormID;
            snapshot.shieldArmorRating = 0.0f;
        }

        g_dualWieldRegistry.emplace(key, snapshot);
        SKSE::log::info("  Weapon combination stored in registry");
        
        // Rebuild cached keys so new weapon is immediately available for summoning
        RebuildCachedKeys();

        SKSE::log::info("TraceActorLoadout: END");
    }

    void PrintEquippedWeapon(RE::ActiveEffect*)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            RE::ConsoleLog::GetSingleton()->Print("No player");
            return;
        }   

        // TraceActorLoadout(player);

    }

    void RegisterNearbyActor(RE::ActiveEffect*, RE::Actor* actor)
    {
        SKSE::log::info("RegisterNearbyActor: called with actor={}", actor ? "valid" : "null");

        if (!actor) {
            SKSE::log::warn("RegisterNearbyActor: actor is null");
            return;
        }

        SKSE::log::info("RegisterNearbyActor: calling TraceActorLoadout");
        TraceActorLoadout(actor);
    }
    
    void SummonMelee(RE::ActiveEffect* effect)
    {
        SKSE::log::info("Trace Weapon (melee) cast");
        if (!effect) {
            SKSE::log::critical("SummonMelee: effect null");
            return;
        }

        auto* target = effect->GetTargetActor();
        SKSE::log::info("SummonMelee: target={}", target ? "valid" : "null");

        // For Self spells, target should be the player and is what you want.
        CastSummonSpell(CombatModality::kMelee);
    }


    void SummonRanged(RE::ActiveEffect* effect)
    {
        SKSE::log::info("Trace Weapon (ranged) cast");
        if (!effect) {
            SKSE::log::critical("SummonRanged: effect null");
            return;
        }

        auto* target = effect->GetTargetActor();
        SKSE::log::info("SummonRanged: target={}", target ? "valid" : "null");

        // For Self spells, target should be the player and is what you want.
        CastSummonSpell(CombatModality::kRanged);
    }


}

// ─────────────────────────────────────────────────────────────
// SKSE entry point
// ─────────────────────────────────────────────────────────────
SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);

    InitializeLogging();
    
    SKSE::log::info("Registering serialization callbacks");
    RegisterSerialization();

    SKSE::GetMessagingInterface()->RegisterListener(
    [](SKSE::MessagingInterface::Message* message)
    {
        switch (message->type)
        {
        case SKSE::MessagingInterface::kDataLoaded:
            SKSE::log::info("TRACING PLUGIN LOADED");
            SKSE::GetPapyrusInterface()->Register(Tracing::RegisterPapyrus);
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
        {
            SKSE::log::info("[Plugin] Game loaded, registering event listeners");

            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                player->AddAnimationGraphEventSink(
                    PlayerAnimationListener::GetSingleton()
                );
            }

            // Register equipment listener to prevent manual unequip
            auto* eventSource = RE::ScriptEventSourceHolder::GetSingleton();
            if (eventSource) {
                eventSource->AddEventSink(PlayerEquipListener::GetSingleton());
                SKSE::log::info("[Plugin] Registered PlayerEquipListener");

                eventSource->AddEventSink(PlayerInventoryListener::GetSingleton());
                SKSE::log::info("[Plugin] Registered PlayerInventoryListener");

                // Register health listener for death alternative mod compatibility
                eventSource->AddEventSink(PlayerHealthListener::GetSingleton());
                SKSE::log::info("[Plugin] Registered PlayerHealthListener");
            }

            // Register periodic update callback for fallback state checking
            SKSE::GetTaskInterface()->AddTask([]() {
                static bool registered = false;
                if (!registered) {
                    SKSE::log::info("[Plugin] Registering periodic state check");
                    registered = true;
                }

                // Schedule recurring check
                auto* task = SKSE::GetTaskInterface();
                if (task) {
                    task->AddTask([]() {
                        PeriodicStateCheck();
                        
                        // Re-schedule self
                        SKSE::GetTaskInterface()->AddTask([]() {
                            PeriodicStateCheck();
                        });
                    });
                }
            });

            break;
        }
        }
    });


    return true;
}