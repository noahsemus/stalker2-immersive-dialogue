ImmersiveDialogue 2.0.1 fixes the one bug everyone hit on release day.

**Fixed:** after talking to an NPC (most often after a trade), some keys stopped working until you reloaded a save: Q and E (quick slots, or lean if you rebound them), L and middle mouse (the headlamp toggled with no animation), and on a controller X, Y, D-pad up and D-pad down. The dialogue's input layer was being left active after the conversation ended. It is now removed the moment the dialogue closes. Thanks to Zenzi0, Saigaiii866, BaneSixEcho and the anonymous reporter for the clear reports.

Nothing else changed. Walking, looking, animations, gestures and trading in dialogue behave exactly as in 2.0.0.

**Install:** drop the zip into Vortex and deploy, or copy the three `zzz_ImmersiveDialogue_20_P.*` files into `Stalker2\Content\Paks\~mods\`, replacing the 2.0.0 ones. If you still have the 1.x DLL installed, remove it (`ue4ss\Mods\ImmersiveDialogueCpp`).

**Still known (cosmetic):** starting a strafe from a standstill inside a dialogue shows about half a second of walk-start before the strafe reads.
