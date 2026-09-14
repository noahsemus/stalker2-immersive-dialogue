ImmersiveDialogue 2.0.0 is a complete rewrite as a pak mod. **UE4SS is no longer required.**

**Install:** drop the zip into Vortex and deploy, or copy the three `zzz_ImmersiveDialogue_20_P.*` files into `Stalker2\Content\Paks\~mods\`. If you had the 1.x DLL installed, remove it (`ue4ss\Mods\ImmersiveDialogueCpp`) so the two don't run together.

**What you get in dialogue**
- Walk with WASD or the left stick, look with the mouse or right stick, from the moment you press "talk".
- Your walking and strafing animations play; in 1.x the body stayed frozen.
- The camera stays where you point it and no longer snaps to the NPC.
- Gestures no longer throw the camera; Skif keeps walking straight ahead while he gestures so the hands stay in frame.
- Controllers use the game's own input system: any pad the game supports works, with the game's sensitivity and dead-zone settings and native DualSense haptics.
- Pair it with a "No Dialogue Zoom" pak from Nexus if you want the FOV zoom-in gone too; this mod leaves config files alone.
- Left stick, W and S no longer scroll the answer list. D-pad, arrow keys and mouse wheel still do.

**Known cosmetic issue:** starting a strafe from a standstill inside a dialogue shows about half a second of walk-start before the strafe reads.

**Removed vs 1.x:** the F6 camera-centering toggle and `config.ini`. Centering is always off in dialogue.

**Compatibility:** this mod overrides `AnimBP_Player`, `BP_Stalker2Character` and `IMC_Dialog` (no config files) and is named to load after other mods. Any mod that overrides the same assets (some weapon-positioning mods ship `AnimBP_Player`) will lose its changes while this is installed. Each game patch needs a rebuild; watch the Nexus page.

Full build documentation for modders is in `BUILD.md` in the repo.
