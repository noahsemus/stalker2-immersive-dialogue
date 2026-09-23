ImmersiveDialogue 2.1.0: no more conflicts with watch and animation mods, arms in dialogue, and your own keybinds.

**For players**
- **Works with watch and animation mods, no patch needed.** The mod no longer replaces the player animation file. Zone Standard Time (tested) and other mods that ship their own copy of it (S-Watch and similar) now work alongside this one, whatever their load order.
- **Arms in dialogue.** Look down in a conversation and Skif's arms are there. His dialogue gestures play on them, at the right height, and follow your view when you look up or down.
- **Your keybinds.** Dialogue movement follows Options > Controls, so AZERTY (ZQSD) and other layouts work, from the first moment of the zoom-in. If you bind "move left" to Q, the game takes Q away from the dialogue's "open upgrade" prompt; give that prompt another key in the same menu.
- Walking, strafing and the body turn in dialogue look the same as outside a conversation.

**Install:** drop `ImmersiveDialogue-v2.1.0.zip` into Vortex, replacing the previous version. The mod is now **six** files: when Vortex asks, tick the six files from `Main` (`zzz_ImmersiveDialogue_20_P.*` and `ImmersiveDialogueStalker2-Windows-NewContent.*`), or **Install All** to add the no-skip-prompt add-on (unchanged). Manual install: copy the six files from `Main` (and optionally the three from `Optional-NoSkipHint`) into `Stalker2\Content\Paks\~mods\`.

**For mod authors**
- `AnimBP_Player` is no longer touched. If you merged our old walk block into yours, it is harmless and can be removed.
- In dialogue the body comes from a post-process anim instance we attach to the player mesh at runtime. The only new contact point: a mod that sets its own post-process anim Blueprint on the player mesh is replaced by ours during dialogue.
- The detection marker `/Game/ImmersiveDialogueCompat/ID_AnimInterface_v1` stays.
