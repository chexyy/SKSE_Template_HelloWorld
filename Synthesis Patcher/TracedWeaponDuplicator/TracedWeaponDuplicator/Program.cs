using Mutagen.Bethesda;
using Mutagen.Bethesda.Synthesis;
using Mutagen.Bethesda.Skyrim;
using System.Threading.Tasks;

namespace TracedWeaponDuplicator
{
    public class Program
    {
        public static async Task<int> Main(string[] args)
        {
            return await SynthesisPipeline.Instance
                .AddPatch<ISkyrimMod, ISkyrimModGetter>(RunPatch)
                .SetTypicalOpen(GameRelease.SkyrimSE, "TracedWeaponDuplicator.esp")
                .Run(args);
        }

        public static void RunPatch(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
        {
            int weaponCount = 0;
            int shieldCount = 0;
            int ammoCount = 0;

            // Duplicate weapons
            foreach (var weaponGetter in state.LoadOrder.PriorityOrder.Weapon().WinningOverrides())
            {
                // Skip weapons that are already bound
                if (weaponGetter.Data?.Flags.HasFlag(WeaponData.Flag.BoundWeapon) ?? false)
                    continue;

                // Skip weapons without proper data
                if (weaponGetter.EditorID == null || weaponGetter.Name == null)
                    continue;

                // Create a duplicate
                var boundWeapon = state.PatchMod.Weapons.DuplicateInAsNewRecord(weaponGetter);
                
                // Modify the duplicate
                boundWeapon.EditorID = weaponGetter.EditorID + "Traced";
                boundWeapon.Name = weaponGetter.Name + " (Traced)";
                
                // Set bound weapon flag and other flags
                if (boundWeapon.Data != null)
                {
                    boundWeapon.Data.Flags |= WeaponData.Flag.BoundWeapon | WeaponData.Flag.CantDrop | WeaponData.Flag.NonPlayable;
                }

                weaponCount++;
            }

            // Duplicate shields
            foreach (var armorGetter in state.LoadOrder.PriorityOrder.Armor().WinningOverrides())
            {
                // Skip non-shields
                if (armorGetter.BodyTemplate == null || !armorGetter.BodyTemplate.FirstPersonFlags.HasFlag(BipedObjectFlag.Shield))
                    continue;

                // Skip armors without proper data
                if (armorGetter.EditorID == null || armorGetter.Name == null)
                    continue;

                // Create a duplicate
                var tracedShield = state.PatchMod.Armors.DuplicateInAsNewRecord(armorGetter);
                
                // Modify the duplicate
                tracedShield.EditorID = armorGetter.EditorID + "Traced";
                tracedShield.Name = armorGetter.Name + " (Traced)";
                
                // Set flags to match bound weapon behavior
                tracedShield.MajorFlags |= Armor.MajorFlag.NonPlayable;

                shieldCount++;
            }

            // Duplicate ammo
            foreach (var ammoGetter in state.LoadOrder.PriorityOrder.Ammunition().WinningOverrides())
            {
                // Skip ammo without proper data
                if (ammoGetter.EditorID == null || ammoGetter.Name == null)
                    continue;

                // Create a duplicate
                var tracedAmmo = state.PatchMod.Ammunitions.DuplicateInAsNewRecord(ammoGetter);
                
                // Modify the duplicate
                tracedAmmo.EditorID = ammoGetter.EditorID + "Traced";
                tracedAmmo.Name = ammoGetter.Name + " (Traced)";
                
                // Set flags to match bound weapon behavior
                tracedAmmo.Flags |= Ammunition.Flag.NonPlayable;

                ammoCount++;
            }
        }
    }
}