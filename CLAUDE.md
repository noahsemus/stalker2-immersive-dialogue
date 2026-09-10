# ImmersiveDialogue — project context for Claude

Purpose: UE4SS C++ mod for **S.T.A.L.K.E.R. 2 (UE5.5)**. During interactive NPC dialogue, injects free WASD movement + raw-mouse look into the player pawn (UClass `PC`, `/Script/Stalker2.PC`) via UE4SS reflection, gated on `PC:IsInStaticDialog()`.

## Layout
```
stalker2-immersive-dialogue\
  CMakeLists.txt                 root (adds RE-UE4SS + ImmersiveDialogueCpp)
  ImmersiveDialogueCpp\
    CMakeLists.txt               builds main.dll, links UE4SS + user32
    dllmain.cpp                  raw-mouse WndProc hook + on_update reflection
  RE-UE4SS\                      cloned; do NOT commit; needs Epic-linked GitHub
  Output\                        CMake build tree (Visual Studio generator)
  BUILD.md                       install/run steps for the end user
  CLAUDE.md                      this file
```

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
STALKER 2 is UE5.5 (LWC), so `FVector` / `FRotator` are `double`-based. dllmain.cpp defines local `FVectorD` / `FRotatorD` structs to match the wire layout when calling `AddMovementInput`, `AddControllerYawInput/PitchInput`, `GetControlRotation`. Do not swap them for `float` structs.

## Tuning knobs (top of `dllmain.cpp`)
- `WALK_SCALE` — movement speed (default 0.35)
- `MOUSE_SENS` — raw counts → yaw/pitch input (default 0.06)

## Install & run
See `BUILD.md` for the end-user copy-the-DLL steps.

## Collaboration workflow (this project only)

Noah is not writing or reading code for this mod. Claude owns the entire dev loop; Noah is the in-game tester.

- **Claude does:** edit `dllmain.cpp`, run `cmake --build Output --config Game__Shipping__Win64 --target ImmersiveDialogue`, copy the resulting `Output\ImmersiveDialogueCpp\Game__Shipping__Win64\main.dll` over the installed copy at `C:\Program Files (x86)\Steam\steamapps\common\S.T.A.L.K.E.R. 2 Heart of Chornobyl\Stalker2\Binaries\Win64\ue4ss\Mods\ImmersiveDialogueCpp\dlls\main.dll`.
- **Noah does:** launch STALKER 2 through Steam, test the mod against an NPC, report symptoms in plain language ("didn't move", "way too fast", "crashed").
- **Do not hand Noah build/install commands.** Just do them. Commands go into `CLAUDE.md`/`BUILD.md` as reference, not into chat as todos for him.
- **Before overwriting `main.dll`,** confirm the game isn't running (`tasklist | grep -i stalker` — the game process is `Stalker2-Win64-Shipping.exe`; `Stalker2ModEditor.exe` is a separate UE editor and does NOT lock the mod DLL).
- **After install,** tell Noah what to look for in-game, what `[ImmDlg]` line in `ue4ss\UE4SS.log` indicates success/failure, and where crash dumps land (`ue4ss\crash_*.dmp`) — no source-level detail unless he asks.
- **Iteration:** when Noah reports back, tweak → build → reinstall → tell him to test again.

This workflow does NOT apply to any of Noah's other projects — default developer-in-the-loop everywhere else.
