# ImmersiveDialogue — project context for Claude

Purpose: UE4SS C++ mod for **S.T.A.L.K.E.R. 2 (UE5.5)**. During interactive NPC dialogue, injects free WASD movement + raw-mouse look into the player pawn (UClass `PC`, `/Script/Stalker2.PC`) via UE4SS reflection, gated on `PC:IsInStaticDialog()`.

Current shipped state: **v1.0** on Nexus (<https://www.nexusmods.com/stalker2heartofchornobyl/mods/2698>) and GitHub releases. First Nexus release cut on 2026-09-11; the gesture-camera fix ("v1.1" work in earlier commits) is folded into that v1.0 tag rather than shipped as a separate version.

## Layout
```
stalker2-immersive-dialogue\
  CMakeLists.txt                 root (adds RE-UE4SS + ImmersiveDialogueCpp)
  ImmersiveDialogueCpp\
    CMakeLists.txt               builds main.dll, links UE4SS + user32 + xinput
    dllmain.cpp                  raw-mouse WndProc hook + on_update reflection
  RE-UE4SS\                      cloned; do NOT commit; needs Epic-linked GitHub
  Output\                        CMake build tree (Visual Studio generator)
  BUILD.md                       install/run steps for the end user
  CLAUDE.md                      this file
```

`config.ini` is generated next to the deployed DLL at first launch (game-side, not in the repo). Contains: `DisableCameraCentering`, `CameraCenteringToggleKey`, `MouseSensitivity`, `GamepadLookSensitivity`, `WalkSpeed`. All optional; missing keys use built-in defaults.

## Build (Shipping — the only config that matches STALKER 2's shipping CRT)
```
cd C:\Users\noahs\OneDrive\Documents\GitHub\stalker2-immersive-dialogue
cmake --build Output --config Game__Shipping__Win64 --target ImmersiveDialogue
```
Output: `Output\ImmersiveDialogueCpp\Game__Shipping__Win64\main.dll`

Full clean reconfigure:
```
Remove-Item -Recurse -Force Output
cmake -S . -B Output
cmake --build Output --config Game__Shipping__Win64 --target ImmersiveDialogue
```

Valid configs (RE-UE4SS's custom triplets — plain `Release`/`Debug` do NOT exist and MSBuild errors with MSB8013):
`Game__Debug__Win64`, `Game__Dev__Win64`, `Game__Test__Win64`, `Game__Shipping__Win64`. Never ship a Debug/Dev mod against Shipping UE4SS.dll — CRT mismatch will crash the game.

## Environment gotchas (all resolved on this machine)

1. **UEPseudo submodule is private (Epic Games gate).** `RE-UE4SS/.gitmodules` points `deps/first/Unreal` at `git@github.com:Re-UE4SS/UEPseudo.git`. Cloning it requires the user's GitHub account to be linked to their Epic Games account. This link is done. Do not attempt to clone RE-UE4SS on a fresh account without doing the link first.

2. **SSH → HTTPS rewrite for submodules.** Set globally:
   ```
   git config --global url."https://github.com/".insteadOf "git@github.com:"
   ```
   Without this, submodule clone tries SSH, and this box has no SSH key on GitHub — clone will die with "Host key verification failed".

3. **Rust is required.** `patternsleuth` (submodule at `deps/first/patternsleuth`) is a Rust crate. Installed via `winget install --id Rustlang.Rustup`; toolchain: `stable-x86_64-pc-windows-msvc`. In fresh PowerShell sessions cargo/rustc may not be on PATH — prepend `$env:USERPROFILE\.cargo\bin` if `rustc --version` fails.

4. **VS 2022** with Desktop C++ workload; MSVC 14.51 toolset; CMake 4.4.3 works. Default generator picks up `Visual Studio 18 Community` (yes, 18 — pre-release channel; still fine).

## Reflection call reference (verified against RE-UE4SS main @ `2bfa839f` = v3.0.1.0.0)

If UE4SS is bumped and dllmain.cpp fails to compile, re-verify these against the headers under `RE-UE4SS/deps/first/`:

| dllmain.cpp usage | Header | Line | Signature |
|---|---|---|---|
| `UObjectGlobals::FindFirstOf(STR("PC"))` | `Unreal/UObjectGlobals.hpp` | 246 | `auto FindFirstOf(const CharType*) -> UObject*` |
| `o->GetFunctionByNameInChain(FName(name))` | `Unreal/UObject.hpp` | 381 | `UFunction* GetFunctionByNameInChain(FName)` |
| `o->ProcessEvent(fn, &p)` | `Unreal/UObject.hpp` | 215 | `void ProcessEvent(UFunction*, void*)` |
| `m_pawn->IsUnreachable()` | `Unreal/UObject.hpp` | 285 | `bool IsUnreachable()` |
| `FName(name, FNAME_Add)` | `Unreal/NameTypes.hpp` | 307 | `explicit FName(const CharType*, EFindName = FNAME_Add, ...)` |
| `Output::send<LogLevel::Verbose>(...)` | `DynamicOutput/Output.hpp` | 228 | `template <int32_t optional_arg, typename FmtArg, typename... FmtArgs> auto send(StringViewType, FmtArg, FmtArgs...)` |
| `start_mod` / `uninstall_mod` exports | `UE4SS/include/Mod/CppMod.hpp` | 22–23 | `CppUserModBase* (*)()` / `void (*)(CppUserModBase*)` |

Header renames to watch for:
- **`Unreal/FName.hpp` → `Unreal/NameTypes.hpp`** (done). The old header no longer exists.
- `Unreal/UClass.hpp` and `Unreal/UFunction.hpp` are forwarding shims that print deprecation warnings; the canonical path is `<Unreal/CoreUObject/UObject/Class.hpp>`. Non-blocking today.

## UE5.5 math types
STALKER 2 is UE5.5 (LWC), so `FVector` / `FRotator` are `double`-based. dllmain.cpp defines local `FVectorD` / `FRotatorD` structs to match the wire layout when calling `AddMovementInput`, `AddControllerYawInput/PitchInput`, `GetControlRotation`, and `GetSocketRotation`. Do not swap them for `float` structs.

## Tuning knobs
Runtime-configurable via the deployed `config.ini` (not source-file constants). Members live at the top of `ImmersiveDialogue` in `dllmain.cpp`:
- `m_walkScale` — fraction of MaxWalkSpeed (default 0.15, `WalkSpeed` in config)
- `m_mouseSens` — multiplied by in-game `MouseSensitivityCoef` (default 0.10, `MouseSensitivity` in config)
- `m_padLookScale` — multiplied by in-game `GamepadSensitivityCoef` (default 0.4, `GamepadLookSensitivity` in config)
- `m_camCenteringDisabled` (default true, `DisableCameraCentering`) and `m_camCenteringToggleVk` (default `VK_F6`, `CameraCenteringToggleKey`)

Constants that are still source-only:
- Gesture detector: `GESTURE_YAW_ENTRY_DEG = 2.5`, `GESTURE_YAW_EXIT_DEG = 0.6`, `GESTURE_TAIL_MS = 1000`, `GESTURE_BASELINE_LPF_ALPHA = 0.02`
- CMC in-dialogue rotation rate: `DIALOGUE_YAW_RATE_DEG_PER_SEC = 180.0`
- Mesh visual body-turn ramp: `MESH_YAW_RATE_DEG_PER_SEC = 240.0` (currently unused — the `ApplyMeshMovementRotation` call is commented out because mesh yaw couples to camera through the socket)

## Gesture-camera fix (v1.0 shipped)
Root cause: STALKER 2's FPS camera pipeline drags `ControlRotation` on every degree of actor yaw change. Default in-dialogue `bOrientRotationToMovement=true` rotates the actor on every strafe input → drags control rotation → camera view swings. Head-bone animation from an NPC gesture compounds through the mesh socket the camera is attached to.

Fix, layered — all in `dllmain.cpp`, all wired up during dialogue only:
1. `ApplyDialogueRotationControl` forces `bOrientRotationToMovement=false` on entry; `RestoreOutsideDialogueRotationControl` restores on exit.
2. `ApplyDialogueRotationRate` slows CMC `RotationRate.Yaw` to 180°/sec as a safety net.
3. `IsGestureAnimatingHead` runs each tick: reads `jnt_camera` socket yaw, subtracts actor yaw and our own mesh-yaw offset, compares to an LPF-drifted baseline with hysteresis (2.5° enter / 0.6° exit / 1000 ms tail).
4. When the detector fires: `ApplyGestureBodyLock` sets `bOrient=false` (belt-and-suspenders), movement input is zeroed, and we engage `SetAbsolute(bAbsRot=true)` on the CameraComponent + a predictive per-tick `RelativeRotation` write. The engage captures the current camera-view-vs-ControlRotation offset and applies it to every write so the view doesn't snap when the lock engages.

Dead ends recorded in memory (`~/.claude/projects/.../memory/project_v1_1_gesture_camera_fix.md`) — read before proposing any camera rework: SetAbsolute over full dialogue, per-tick RelativeRotation without prediction, mesh RelativeRotation ramp, StopMovementImmediately, defensive re-apply of pawn control flags.

## Install & run
See `BUILD.md` for the end-user copy-the-DLL steps.

### Vortex quirk to remember
The STALKER 2 Vortex extension (Nexus mod 958, ChemBoy1) explicitly strips `enabled.txt` from UE4SS DLL archives (see extension source at `%APPDATA%\Vortex\plugins\STALKER 2 HoC Vortex Extension .../index.js`, `installDll`). It manages `mods.txt` via its **UE4SS Load Order** tab instead — but the tab only serializes to `mods.txt` when the user interacts with it (toggle, reorder, save). So a Vortex install deploys the DLL correctly but the mod stays inert until the user visits that tab. The README's Install → Vortex section walks users through this.

Ship the ZIP with the flat `<ModName>/dlls/main.dll` structure (plus an `enabled.txt` marker for manual installers — Vortex will strip it, that's fine).

## Collaboration workflow (this project only)

Noah is not writing or reading code for this mod. Claude owns the entire dev loop; Noah is the in-game tester.

- **Claude does:** edit `dllmain.cpp`, run `cmake --build Output --config Game__Shipping__Win64 --target ImmersiveDialogue`, copy the resulting `Output\ImmersiveDialogueCpp\Game__Shipping__Win64\main.dll` over the installed copy at `C:\Program Files (x86)\Steam\steamapps\common\S.T.A.L.K.E.R. 2 Heart of Chornobyl\Stalker2\Binaries\Win64\ue4ss\Mods\ImmersiveDialogueCpp\dlls\main.dll`.
- **Noah does:** launch STALKER 2 through Steam, test the mod against an NPC, report symptoms in plain language ("didn't move", "way too fast", "crashed").
- **Do not hand Noah build/install commands.** Just do them. Commands go into `CLAUDE.md`/`BUILD.md` as reference, not into chat as todos for him.
- **Before overwriting `main.dll`,** confirm the game isn't running (`tasklist | grep -i stalker` — the game process is `Stalker2-Win64-Shipping.exe`; `Stalker2ModEditor.exe` is a separate UE editor and does NOT lock the mod DLL).
- **After install,** tell Noah what to look for in-game, what `[ImmDlg]` line in `ue4ss\UE4SS.log` indicates success/failure, and where crash dumps land (`ue4ss\crash_*.dmp`) — no source-level detail unless he asks.
- **Iteration:** when Noah reports back, tweak → build → reinstall → tell him to test again.

This workflow does NOT apply to any of Noah's other projects — default developer-in-the-loop everywhere else.
