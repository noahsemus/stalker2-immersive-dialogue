# ImmersiveDialogue

A UE4SS C++ mod for **S.T.A.L.K.E.R. 2: Heart of Chornobyl** that lets you move
and look around freely during interactive NPC dialogue.

Vanilla STALKER 2 pins the camera on the NPC and locks you in place the moment
you press "talk". This mod restores agency: WASD/left-stick walks, mouse/right-
stick looks, and the camera stays where you point it.

Nexus page: <https://www.nexusmods.com/stalker2heartofchornobyl/mods/2698>

Slava Ukraini.

## Features

- **Free movement in dialogue.** WASD (keyboard) and left stick (controller)
  walk Skif at a natural pace, camera-relative. Right stick and mouse look work
  the same as normal gameplay.
- **Sensitivity honors your in-game settings.** Reads `MouseSensitivityCoef`,
  `GamepadSensitivityCoef`, `InvertMouseYAxis`, and `GamepadInvertX/YAxis` from
  your live `AppliedSettingsWin64.cfg`. Per-mod base multipliers are also
  exposed in `config.ini` if you want to tune.
- **Camera stays free.** The vanilla dialogue camera modifier that yanks your
  view onto the NPC is disabled while in dialogue (toggle via F6 by default,
  key rebindable in `config.ini`).
- **Left stick doesn't scroll dialogue options.** The two `Gamepad_LeftStick_Up/Down`
  bindings on `IA_UI_Dialog_SelectAnswer` are neutralized at runtime, so you can
  walk with the left stick without accidentally cycling through NPC responses.
  D-pad Up/Down and the confirm button work as normal.
- **Wwise footsteps fire during in-dialogue walking.** STALKER 2's anim graph
  suppresses foot notifies in dialogue; the mod triggers the appropriate
  `SFX_Skif_Footsteps` event on cadence so you still hear your steps.
- **Camera stays stable when NPC gestures.** In-dialogue upper-body gestures
  (Skif waving his hand, shrugging, etc.) animate the head bone that the
  camera socket is attached to — which in vanilla produces a wild camera swing
  when combined with strafe input. The mod fixes this with a four-layer
  approach: force `bOrientRotationToMovement=false` so actor rotation never
  drags the camera's control rotation, detect an active gesture via the
  `jnt_camera` socket yaw with adaptive-baseline hysteresis, cut movement
  input for the gesture window, and engage `SetAbsolute(rot=true)` on the
  camera component (with a per-tick predictive `RelativeRotation` write that
  preserves the current view offset) so the head bone can't push the view.
  See "Known limitations" for the small tradeoff.

## Companion mods (recommended)

- **No Dialogue Zoom** — kills the FOV zoom-in on dialogue entry. The zoom is
  driven by `DialogFOVDefault` in `CoreVariables.cfg`; a config-based pak mod
  is the only clean fix (runtime FOV overrides fight the game and flicker).
  Any of the Nexus variants work — pick one matching your normal in-game FOV:
  - <https://www.nexusmods.com/stalker2heartofchornobyl/mods/71>
  - <https://www.nexusmods.com/stalker2heartofchornobyl/mods/1499>
  - <https://www.nexusmods.com/stalker2heartofchornobyl/mods/1933>

  Drop the `.pak` into `<GAME>\Stalker2\Content\Paks\~mods\` alongside this mod.

- **RE-UE4SS Compatibility Fix for Update 2.0** — required while on STALKER 2
  Update 2.0 / UE5.5 for any UE4SS install to load at all.
  <https://www.nexusmods.com/stalker2heartofchornobyl/mods/2341>

## Known limitations

- **Movement is suppressed while a gesture animation is playing.** During the
  ~1s an NPC-triggered upper-body gesture is playing, WASD / left-stick input
  is dropped and the character stops walking. Mouse / right-stick look works
  normally throughout. The alternative was the wild camera swing the vanilla
  gesture-plus-strafe compound produced.
- **F confirm glyph on the highlighted dialogue option may render slightly
  higher than expected.** Purely cosmetic; the confirm key still works normally.
  Caused by the runtime `IMC_Dialog` patch keeping neutralized entries in the
  mapping array instead of removing them (removal corrupted UE runtime state
  and crashed the PDA on v1.0-rc).

## Requirements

- STALKER 2 (Steam or Epic install)
- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) installed to
  `Stalker2\Binaries\Win64\ue4ss\`. Built and tested against RE-UE4SS
  v3.0.1.0.0; v3.0.x branches should be ABI-compatible.
- On STALKER 2 Update 2.0 / UE5.5: also install the
  [RE-UE4SS Compatibility Fix](https://www.nexusmods.com/stalker2heartofchornobyl/mods/2341)
  so UE4SS itself will load.
- To build from source: Visual Studio 2022 with the "Desktop development
  with C++" workload, CMake 3.22+, Rust (for the `patternsleuth` submodule
  of RE-UE4SS), and a GitHub account linked to Epic Games (RE-UE4SS's
  `UEPseudo` submodule is gated behind that link).

## Install

### Vortex

1. Grab the latest `ImmersiveDialogue-vX.Y.Z.zip` (from Nexus or this repo's Releases page)
   and drop it into Vortex.
2. Deploy.
3. Open Vortex's **UE4SS Load Order** tab. `ImmersiveDialogueCpp` will appear
   in the list, enabled by default. Toggle it off then on (or reorder any
   entry) to force the Vortex extension to write the mod into UE4SS's
   `mods.txt`. This is a one-time step required by the STALKER 2 Vortex
   extension — it defers `mods.txt` writes to that tab rather than firing them
   on install.
4. Launch the game.

### Manual

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
   Or drop an empty `enabled.txt` next to the `dlls/` folder.

## Build from source

See [BUILD.md](BUILD.md).

## Configuration

`config.ini` lives next to the DLL and is created with defaults on first
launch. All keys are optional; omitted keys use the built-in defaults.

```ini
DisableCameraCentering=true
CameraCenteringToggleKey=F6
MouseSensitivity=0.10
GamepadLookSensitivity=0.4
WalkSpeed=0.15
```

- `DisableCameraCentering` — whether the vanilla NPC-centering camera modifier
  is disabled during dialogue. Toggled in-game with `CameraCenteringToggleKey`
  and persisted back to the file.
- `CameraCenteringToggleKey` — the hotkey used to toggle the above. Accepts
  key names like `F1`-`F24`, `A`-`Z`, `0`-`9`, `Home`/`End`/`PageUp`/
  `PageDown`/`Insert`/`Delete`/`Space`/`Tab`/`Backspace`/`Enter`/`Escape`/
  `CapsLock`/`NumLock`/`ScrollLock`/`Pause`, or a raw Win32 virtual-key hex
  code (e.g. `0x71`). Default `F6`. `F5` is the game's quicksave key — don't
  rebind to that.
- `MouseSensitivity` / `GamepadLookSensitivity` — base multipliers for
  mouse / right-stick look while in dialogue. The final applied sensitivity
  multiplies these by your in-game `MouseSensitivityCoef` /
  `GamepadSensitivityCoef`, so the in-game sliders still work as expected.
- `WalkSpeed` — fraction of the character's normal walk speed used while
  moving in dialogue. Default is a natural in-dialogue pace.

## Compatibility

- No known conflicts with pak mods, gameplay overhauls, or graphics mods —
  the mod only touches player-pawn properties during a dialogue interaction
  (`PC:IsInStaticDialog()==true`).
- Confirmed working alongside NWA (mod
  [781](https://www.nexusmods.com/stalker2heartofchornobyl/mods/781)) with its
  `HookEngineTick=1` UE4SS-settings.ini per community report on the mod page.
- Uses UE4SS's `on_update` callback, which needs at least one tick hook enabled
  in `UE4SS-settings.ini`. All standard configurations have this.

## Credits

Made by Noah Semus with heavy pair-programming from Claude (Anthropic).
STALKER 2 is developed by GSC Game World in Kyiv, Ukraine.

## License

MIT — see [LICENSE](LICENSE).
