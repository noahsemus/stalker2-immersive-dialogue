ImmersiveDialogue 2.0.3 makes the mod stand down during cutscenes.

**Fixed (hopefully — please report back):** several players reported a broken camera in story cutscenes: the view stuck facing one way in the prologue scenes (the dog, waking up next to Richter), or following the scripted path but unable to tilt up or down (very visible in "Back to the Slag Heap" and the DLC's "Distant Mirage"). Those scenes run through the game's dialogue system, so the mod was treating them as conversations and taking over the camera. The mod now only acts in a conversation when the game is **not** running a cinematic and has **not** taken look control away from the player.

I could not reproduce the problem on my own install, so this is a best-effort fix: if you still see it on 2.0.3, please say which scene, and whether it survives a fresh game start.

Normal conversations are unchanged: free movement, look, walking animation, no camera centering.

**Install:** drop `ImmersiveDialogue-v2.0.3.zip` into Vortex, replacing 2.0.2. Because the zip holds two paks, Vortex lists the files and asks which to install: tick the three `zzz_ImmersiveDialogue_20_P` files for the mod alone, or **Install All** for the mod plus the no-skip-prompt add-on (unchanged from 2.0.2). Manual install: copy the three files from `Main` (and, if you want the add-on, the three from `Optional-NoSkipHint`) into `Stalker2\Content\Paks\~mods\`.

**Still known (cosmetic):** Skif's arms are not visible in dialogue except while he gestures.
