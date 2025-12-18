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
    void RegisterNearbyActor(RE::ActiveEffect* effect);
}

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
};


// Stores the descriptive data associated with a weapon combination.
// This data may grow over time and does not participate in identity.
struct DualWieldSnapshot
{
    WeaponSnapshot right;
    WeaponSnapshot left;
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

// ─────────────────────────────────────────────────────────────
// Variable Initialization
// ─────────────────────────────────────────────────────────────

// Global registry mapping weapon combinations to their snapshots
using DualWieldRegistry =
    std::unordered_map<WeaponCombinationKey, DualWieldSnapshot, WeaponCombinationKeyHash>;

static DualWieldRegistry g_dualWieldRegistry;

// FormID for no weapon (empty hand)
constexpr RE::FormID kEmptyHandFormID = 0;

// Scanned actors
static std::unordered_set<RE::FormID> g_seenActors;

// ─────────────────────────────────────────────────────────────
// Helper Functions
// ─────────────────────────────────────────────────────────────

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

    if (rightRanged && leftRanged) {
        return CombatModality::kRanged;
    }
    if (!rightRanged && !leftRanged) {
        return CombatModality::kMelee;
    }
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

float ComputeCombinedDamage(const DualWieldSnapshot& snapshot)
{
    float right =
        snapshot.right.baseDamage +
        snapshot.right.enchantDamage;

    float left =
        snapshot.left.baseDamage +
        snapshot.left.enchantDamage;

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

WeaponSnapshot BuildWeaponSnapshot(RE::TESObjectWEAP* weapon)
{
    WeaponSnapshot snapshot{};
    snapshot.formID        = weapon->GetFormID();
    snapshot.weaponType    = weapon->GetWeaponType();
    snapshot.baseDamage    = weapon->GetAttackDamage();
    snapshot.enchantDamage = ComputeEnchantmentDamage(weapon);
    return snapshot;
}

inline const char* SafeName(const RE::TESForm* form, const char* fallback = "<no name>")
{
    if (!form) {
        return fallback;
    }
    const char* n = form->GetName();
    return (n && n[0]) ? n : fallback;
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
        
        return true;
    }

    // ─────────────────────────────────────────────────────────
    // Instance native (called from ActiveMagicEffect)
    // ─────────────────────────────────────────────────────────
    void TraceActorLoadout(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        if (actor->IsDead()) {
            return;
        }

        if (!actor->Is3DLoaded()) {
            return;
        }

        auto* process = actor->GetActorRuntimeData().currentProcess;
        if (!process) {
            return;
        }

        if (!process->middleHigh) {
            return;
        }

        auto* rightObj = actor->GetEquippedObject(false);
        auto* leftObj  = actor->GetEquippedObject(true);

        auto* rightWeap = rightObj ? rightObj->As<RE::TESObjectWEAP>() : nullptr;
        if (!rightWeap) {
            return; // invariant: right-hand weapon required
        }

        auto* leftWeap  = leftObj ? leftObj->As<RE::TESObjectWEAP>() : nullptr;
        auto* leftArmor = leftObj ? leftObj->As<RE::TESObjectARMO>() : nullptr;

        bool hasShield = leftArmor && leftArmor->IsShield();

        RE::FormID leftFormID =
            leftWeap  ? leftWeap->GetFormID() :
            hasShield ? leftArmor->GetFormID() :
                        kEmptyHandFormID;

        WeaponCombinationKey key{
            rightWeap->GetFormID(),
            leftFormID
        };

        // Deduplication
        if (g_dualWieldRegistry.contains(key)) {
            return;
        }

        DualWieldSnapshot snapshot{
            BuildWeaponSnapshot(rightWeap),
            leftWeap ? BuildWeaponSnapshot(leftWeap) : WeaponSnapshot{}
        };

        g_dualWieldRegistry.emplace(key, snapshot);

        // ─────────────────────────────────────────────
        // Console output (shared for Player + NPCs)
        // ─────────────────────────────────────────────

        std::string msg = "New combo recorded: ";

        // Identify actor
        if (actor->IsPlayerRef()) {
            msg += "[Player] ";
        } else {
            msg += "[NPC: ";
            msg += SafeName(actor, "<unnamed NPC>");
            msg += "] ";
        }

        msg += "R=";
        msg += SafeName(rightWeap, "<unnamed weapon>");
        msg += " | L=";

        if (leftWeap) {
            msg += SafeName(leftWeap, "<unnamed weapon>");
        } else if (hasShield) {
            msg += SafeName(leftArmor, "<unnamed shield>");
        } else {
            msg += "Empty";
        }


        RE::ConsoleLog::GetSingleton()->Print(msg.c_str());

        // ─────────────────────────────────────────────
        // Bias diagnostics
        // ─────────────────────────────────────────────

        float shieldAR = hasShield ? leftArmor->GetArmorRating() : 0.0f;
        float bias = ComputeBias(key, snapshot, shieldAR);

        float combinedDamage = ComputeCombinedDamage(snapshot);
        float effective = combinedDamage * bias;

        std::string dbg = "Bias=";
        dbg += std::to_string(bias);
        dbg += " | CombinedDmg=";
        dbg += std::to_string(combinedDamage);
        dbg += " | Effective=";
        dbg += std::to_string(effective);

        RE::ConsoleLog::GetSingleton()->Print(dbg.c_str());
    }


    // void RegisterNearbyActor(RE::ActiveEffect* effect)
    // {
    //     RE::ConsoleLog::GetSingleton()->Print("Tracing: RegisterNearbyActor called");
    //     auto* actor = effect ? effect->GetTargetActor() : nullptr;
    //     if (!actor) {
    //     return;
    //     }

    //     if (actor->IsDead()) {
    //         return;
    //     }

    //     if (actor->IsPlayerRef()) {
    //         return;
    //     }

    //     if (!actor->Is3DLoaded()) {
    //         return;
    //     }

    //     TraceActorLoadout(actor);
    // }

    void RegisterNearbyActor(RE::ActiveEffect* effect)
    {
        SKSE::log::critical("STEP A: instance native entered");

        if (!effect) {
            SKSE::log::critical("STEP A1: effect null");
            return;
        }

        auto* actor = effect->GetTargetActor();
        if (!actor) {
            SKSE::log::critical("STEP A2: actor null");
            return;
        }

        SKSE::log::critical("STEP B: calling TraceActorLoadout");

        TraceActorLoadout(actor);

        SKSE::log::critical("STEP C: returned from TraceActorLoadout");
    }


    // void RegisterNearbyActor(RE::Actor* actor)
    // {
    //     RE::ConsoleLog::GetSingleton()->Print("RegisterNearbyActor ENTERED");

    //     if (!actor) {
    //         RE::ConsoleLog::GetSingleton()->Print("Actor was null");
    //         return;
    //     }

    //     RE::ConsoleLog::GetSingleton()->Print(actor->GetName());
    // }


    void PrintEquippedWeapon(RE::ActiveEffect*)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            RE::ConsoleLog::GetSingleton()->Print("No player");
            return;
        }

        TraceActorLoadout(player);

        // original
        // // false = right hand, true = left hand
        // auto* rightObj = player->GetEquippedObject(false);
        // auto* leftObj   = player->GetEquippedObject(true);

        // // resolve what is equipped in each hand
        // auto* rightWeap = rightObj ? rightObj->As<RE::TESObjectWEAP>() : nullptr;
        // auto* leftWeap  = leftObj ? leftObj->As<RE::TESObjectWEAP>() : nullptr;
        // auto* leftArmor = leftObj ? leftObj->As<RE::TESObjectARMO>() : nullptr;

        // bool hasShield = leftArmor && leftArmor->IsShield();
        
        // // no left or right equipment
        // if (!rightWeap) {
        //     RE::ConsoleLog::GetSingleton()->Print("No relevant equipment");
        //     return;
        // }

        // // resolve left-hand form ID
        // RE::FormID leftFormID =
        // leftWeap  ? leftWeap->GetFormID() :
        // hasShield ? leftArmor->GetFormID() :
        //             kEmptyHandFormID;

        // WeaponCombinationKey key{
        //     rightWeap ? rightWeap->GetFormID() : kEmptyHandFormID,
        //     leftFormID
        // };

        // // Deduplication check
        // if (g_dualWieldRegistry.contains(key)) {
        //     RE::ConsoleLog::GetSingleton()->Print("Dual-wield combo already known");
        //     return;
        // }

        // // Build snapshot
        // DualWieldSnapshot snapshot{
        //     rightWeap ? BuildWeaponSnapshot(rightWeap) : WeaponSnapshot{},
        //     leftWeap  ? BuildWeaponSnapshot(leftWeap)  : WeaponSnapshot{}
        // };

        // // Store
        // g_dualWieldRegistry.emplace(key, snapshot);

        // // Confirmation
        // std::string msg = "New combo recorded: R=";

        // msg += rightWeap ? rightWeap->GetName() : "Empty";
        // msg += " | L=";

        // if (leftWeap) {
        //     msg += leftWeap->GetName();
        // } else if (hasShield) {
        //     msg += leftArmor->GetName();
        // } else {
        //     msg += "Empty";
        // }

        // RE::ConsoleLog::GetSingleton()->Print(msg.c_str());

        // // Verification of Bias
        // float shieldAR = hasShield ? leftArmor->GetArmorRating() : 0.0f;
        // float bias = ComputeBias(key, snapshot, shieldAR);

        // float combinedDamage = ComputeCombinedDamage(snapshot);
        // float effective = combinedDamage * bias;

        // std::string dbg = "Bias=";
        // dbg += std::to_string(bias);
        // dbg += " | CombinedDmg=";
        // dbg += std::to_string(combinedDamage);
        // dbg += " | Effective=";
        // dbg += std::to_string(effective);

        // RE::ConsoleLog::GetSingleton()->Print(dbg.c_str());

    }

}

// ─────────────────────────────────────────────────────────────
// SKSE entry point
// ─────────────────────────────────────────────────────────────
SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);

    SKSE::GetMessagingInterface()->RegisterListener(
        [](SKSE::MessagingInterface::Message* message)
        {

            InitializeLogging();
            
            switch (message->type)
            {
            case SKSE::MessagingInterface::kDataLoaded:
                RE::ConsoleLog::GetSingleton()->Print("Tracing plugin loaded");
                SKSE::log::critical("TRACING PLUGIN LOADED");
                SKSE::GetPapyrusInterface()->Register(Tracing::RegisterPapyrus);
                break;
            }
        });

    return true;
}