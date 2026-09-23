ImmersiveDialogue v2.1.0 for S.T.A.L.K.E.R. 2: Heart of Chornobyl
Free movement, look and walking animation during NPC dialogue. No UE4SS required.

WHAT IS IN THIS ZIP
  Main\                  the mod itself (required), six files:
      zzz_ImmersiveDialogue_20_P.pak / .ucas / .utoc
      ImmersiveDialogueStalker2-Windows-NewContent.pak / .ucas / .utoc
  Optional-NoSkipHint\   optional add-on: hides the "press X to skip" prompt in
      zzz_ImmersiveDialogueNoSkipHint_20_P.pak / .ucas / .utoc
                         dialogue (with free look, moving or looking around kept
                         making it pop up). The skip key still works without it.

INSTALL (Vortex)
  Drop the zip into Vortex, replacing the previous version. Vortex lists the
  files and asks which to install:
    - tick the six files from Main for the mod alone, or
    - "Install All" for the mod plus the no-skip-prompt add-on.
  Deploy. Done. To change your choice later, reinstall the mod from the zip.

INSTALL (manual)
  Copy the six files from Main\ (and, if you want the add-on, the three from
  Optional-NoSkipHint\) into
      <game>\Stalker2\Content\Paks\~mods\
  (create the ~mods folder if it does not exist). Don't rename them. If you are
  updating from 2.0.x, just copy over the old files.

UNINSTALL
  Delete the files you copied.

KEYBINDS
  Dialogue movement follows your bindings in Options > Controls (AZERTY and
  other layouts work). If you bind "move left" to Q, the game unbinds the
  dialogue's "open upgrade" prompt from Q; give it another key in the same menu.

RECOMMENDED COMPANION
  This mod does not change the dialogue FOV zoom. Pair it with a Nexus
  "No Dialogue Zoom" pak (mods 71, 1499 or 1933) if you want that gone too.

COMPATIBILITY
  From 2.1.0 the mod no longer replaces the player animation file
  (AnimBP_Player): watch, weapon and animation mods that ship their own copy
  (Zone Standard Time and others) work alongside with no patch. It still
  overrides two game assets, BP_Stalker2Character and IMC_Dialog (the add-on
  overrides one UI widget, W_SkipHintView). No config files.

Mod authors: the GitHub README has a "Compatibility for mod authors" section.

Source and build instructions: https://github.com/noahsemus/stalker2-immersive-dialogue
