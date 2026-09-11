# ImmersiveDialogue

A UE4SS C++ mod for **S.T.A.L.K.E.R. 2: Heart of Chornobyl** that lets you move
and look around freely during interactive NPC dialogue.

Vanilla STALKER 2 pins the camera on the NPC and locks you in place the moment
you press "talk". This mod restores agency: WASD/left-stick walks, mouse/right-
stick looks, and the camera stays where you point it.

Slava Ukraini.

## Features

- **Free movement in dialogue.** WASD (keyboard) and left stick (controller)
  walk Skif at a natural pace, camera-relative. Right stick and mouse look work
  the same as normal gameplay.
- **Sensitivity is dynamic.** Reads `MouseSensitivityCoef`, `GamepadSensitivityCoef`,
  `InvertMouseYAxis`, and `GamepadInvertX/YAxis` from your live
  `AppliedSettingsWin64.cfg` — no per-mod slider to configure.
- **Camera stays free.** The vanilla dialogue camera modifier that yanks your
  view onto the NPC is disabled every tick (toggle via F6, persisted to
  `config.ini`).
- **Left stick doesn't scroll dialogue options.** The two `Gamepad_LeftStick_Up/Down`
  bindings on `IA_UI_Dialog_SelectAnswer` are neutralized at runtime, so you can
  walk with the left stick without accidentally cycling through NPC responses.
  D-pad Up/Down and the confirm button work as normal.
- **Wwise footsteps fire during in-dialogue walking.** STALKER 2's anim graph
  suppresses foot notifies in dialogue; the mod triggers the appropriate
  `SFX_Skif_Footsteps` event on cadence so you still hear your steps.

## Companion mods (recommended)

- **No Dialogue Zoom** — kills the FOV zoom-in on dialogue entry. The zoom is
  driven by `DialogFOVDefault` in `CoreVariables.cfg`; a config-based pak mod
  is the only clean fix (runtime FOV overrides fight the game and flicker).
  Any of the Nexus variants work — pick one matching your normal in-game FOV:
  - https://www.nexusmods.com/stalker2heartofchornobyl/mods/71
  - https://www.nexusmods.com/stalker2heartofchornobyl/mods/1499
  - https://www.nexusmods.com/stalker2heartofchornobyl/mods/1933

Drop the `.pak` into
`<GAME>\Stalker2\Content\Paks\~mods\` alongside this mod.

## Known issues in v1.0

- **Dialog gestures still move the camera.** When Skif does a body gesture
  (hand wave, shrug, etc.), his `jnt_camera` bone moves and drags the camera
  with it, briefly stealing control from your mouse/right stick. A runtime
  fix was implemented but crashed the game around the PDA-open flow and was
  removed for stability. This is the top target for v1.1.
- **F confirm glyph on the highlighted dialogue option may render slightly
  higher than expected.** Purely cosmetic; the confirm key still works normally.
  Caused by the runtime `IMC_Dialog` patch keeping neutralized entries in the
  mapping array instead of removing them (removal corrupted UE runtime state
  and crashed the PDA).

## Requirements

- STALKER 2 (Steam or Epic install)
- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) installed to
  `Stalker2\Binaries\Win64\ue4ss\`
- To build from source: Visual Studio 2022 with the "Desktop development
  with C++" workload, CMake 3.22+, Rust (for the `patternsleuth` submodule
  of RE-UE4SS), and a GitHub account linked to Epic Games (RE-UE4SS's
  `UEPseudo` submodule is gated behind that link).

## Install (pre-built release)

1. Grab `main.dll` from the latest release on this repo.
2. Copy it to:
   ```
   <GAME>\Stalker2\Binaries\Win64\ue4ss\Mods\ImmersiveDialogueCpp\dlls\main.dll
   ```
3. Add this line to `<GAME>\Stalker2\Binaries\Win64\ue4ss\Mods\mods.txt`
   (above any keybind mods):
   ```
   ImmersiveDialogueCpp : 1
   ```

## Build from source

See [BUILD.md](BUILD.md).

## Configuration

`config.ini` lives next to the DLL. Currently one key:

```ini
DisableCameraCentering=true
```

`F6` in-game toggles it and rewrites the file.

## Credits

Made by Noah Semus with heavy pair-programming from Claude (Anthropic).
STALKER 2 is developed by GSC Game World in Kyiv, Ukraine.

## License

MIT — see [LICENSE](LICENSE).
