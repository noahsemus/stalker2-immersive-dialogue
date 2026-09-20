ImmersiveDialogue 2.0.4 is a compatibility release. Nothing changes in how dialogue feels.

**For players**
- Camera handling during dialogue moved out of the player animation file into the player character. Same behaviour, but it keeps working when another mod's copy of the animation file is the one in use (Zone Standard Time ships one).
- With **Zone Standard Time 1.0.5** installed: the watch works and the dialogue camera is handled correctly, but Skif's legs won't animate while walking in dialogue until ZST updates. Its check for this mod only recognises the 2.0.2 build. The author has the details.
- The cutscene fix from 2.0.3 is included. If a story cutscene's camera still misbehaves, please report which scene and whether it survives a fresh game start.

**For mod authors**
- Our part of `AnimBP_Player` is now only the walk wiring and is frozen from this release, so it only needs merging once.
- New stable marker asset to detect the mod: `/Game/ImmersiveDialogueCompat/ID_AnimInterface_v1`. Don't detect via the `__ModKitWwiseCookAnchor_…` asset; its name changes every build.
- The README has a new "Compatibility for mod authors" section with the exact block to merge.

**Install:** drop `ImmersiveDialogue-v2.0.4.zip` into Vortex, replacing the previous version. Because the zip holds two paks, Vortex lists the files and asks which to install: tick the three `zzz_ImmersiveDialogue_20_P` files for the mod alone, or **Install All** for the mod plus the no-skip-prompt add-on (unchanged). Manual install: copy the three files from `Main` (and, if you want the add-on, the three from `Optional-NoSkipHint`) into `Stalker2\Content\Paks\~mods\`.

**Still known (cosmetic):** Skif's arms are not visible in dialogue except while he gestures.
