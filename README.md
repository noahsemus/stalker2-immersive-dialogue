# ImmersiveDialogue

A UE4SS C++ mod for **S.T.A.L.K.E.R. 2: Heart of Chornobyl** that restores free movement and mouse look during interactive NPC dialogue.

STALKER 2's default dialogue mode locks the player in place. This mod injects `AddMovementInput`, `AddControllerYawInput` and `AddControllerPitchInput` on the player pawn every frame while `PC::IsInStaticDialog()` is true, and hooks Win32 raw mouse + XInput so you can walk around and look while conversing.

## Features
- **WASD movement** during dialogue, scaled to a walking pace.
- **Raw-mouse look**, release-fraction smoothed, dynamically scaled to the game's `MouseSensitivityCoef` and `InvertMouseYAxis` from `AppliedSettingsWin64.cfg`.
- **Controller support** via XInput — left stick moves, right stick looks. `GamepadSensitivityCoef` and `GamepadInvertX/Y` honored dynamically.
- **W/A/S/D swallowed** at the WndProc layer so the dialogue option list doesn't scroll while you walk.
- **Escape hold ≥ 500ms** exits the dialogue via a synthetic Escape.
- **Escape tap** attempts to open the game's pause menu overlay (experimental — STALKER 2 gates pause during dialogue).
- **Footsteps forced on** each frame (best effort; STALKER 2's animation graph may still mute them).

## Requirements
- STALKER 2 (Steam / Epic install)
- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) installed to `Stalker2\Binaries\Win64\ue4ss\`
- To **build** from source: Visual Studio 2022 (Desktop C++ workload), CMake 3.22+, Rust (for the `patternsleuth` submodule), and a GitHub account linked to Epic Games (for the private `UEPseudo` submodule)

## Install (pre-built)
Grab `main.dll` from a release, then:

    <GAME>\Stalker2\Binaries\Win64\ue4ss\Mods\ImmersiveDialogueCpp\dlls\main.dll

Add `ImmersiveDialogueCpp : 1` to `<GAME>\Stalker2\Binaries\Win64\ue4ss\Mods\mods.txt`.

## Build from source
See [BUILD.md](BUILD.md).

## License
MIT — see [LICENSE](LICENSE).
