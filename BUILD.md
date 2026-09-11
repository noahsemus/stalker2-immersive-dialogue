# ImmersiveDialogue (C++) — build & install

## Prereqs (Windows 11)
- Visual Studio 2022 Community, with the **"Desktop development with C++"** workload.
  (This is the full VS IDE, NOT VS Code.)
- CMake 3.22+  (winget install Kitware.CMake)
- git

## 1. Get the UE4SS source (needed to compile against)
In an empty working folder (e.g. C:\dev\immdlg\), put this project so it looks like:

    immdlg\
      CMakeLists.txt            <- from this zip (root)
      ImmersiveDialogueCpp\     <- from this zip
        dllmain.cpp
        CMakeLists.txt
      RE-UE4SS\                 <- you clone this next

Clone RE-UE4SS INTO that folder:

    cd C:\dev\immdlg
    git clone --recursive https://github.com/UE4SS-RE/RE-UE4SS.git

(If it forgets submodules: `cd RE-UE4SS && git submodule update --init --recursive`)

## 2. Configure + build (Shipping)
    cd C:\dev\immdlg
    cmake -S . -B Output
    cmake --build Output --config Game__Shipping__Win64 --target ImmersiveDialogue

RE-UE4SS does NOT use the standard `Release`/`Debug` configs — it defines its own
triplets: `Game__Debug__Win64`, `Game__Dev__Win64`, `Game__Test__Win64`,
`Game__Shipping__Win64`. STALKER 2 ships in Shipping, so `Game__Shipping__Win64`
is the only config that matches its C runtime — mixing configs will crash the game.

Requires Rust (`winget install --id Rustlang.Rustup`) — RE-UE4SS's `patternsleuth`
submodule is a Rust crate and CMake configure will refuse to proceed without `rustc`.

Output path:
    Output\ImmersiveDialogueCpp\Game__Shipping__Win64\main.dll

## 3. Install
Copy that `main.dll` to:

    ...\S.T.A.L.K.E.R. 2 Heart of Chornobyl\Stalker2\Binaries\Win64\ue4ss\Mods\ImmersiveDialogueCpp\dlls\main.dll

Enable it: add a line to `ue4ss\Mods\mods.txt` ABOVE the keybind mods:

    ImmersiveDialogueCpp : 1

(Disable the Lua v1.0 ImmersiveDialogue while testing this so they don't both run.)

## 4. Run
Launch the game, talk to an NPC: WASD to move, MOUSE to look. Check ue4ss\UE4SS.log
for `[ImmDlg]` lines. Expect a compile-fix pass first — send me the exact compiler
errors and I'll correct the reflection API calls against your headers.

## 5. Suggested companion mod: "No Dialogue Zoom"
This mod does not touch the dialogue FOV zoom — that value lives in
`CoreVariables.cfg` as `DialogFOVDefault` and every runtime override loses a frame
war with the game's own writes. Install one of the Nexus "No Dialogue Zoom" paks
alongside this DLL (pick the variant matching your normal in-game FOV):

- https://www.nexusmods.com/stalker2heartofchornobyl/mods/71
- https://www.nexusmods.com/stalker2heartofchornobyl/mods/1933

Drop the .pak in `...\S.T.A.L.K.E.R. 2 Heart of Chornobyl\Stalker2\Content\Paks\~mods\`.

## Tuning (top of dllmain.cpp)
- WALK_SCALE  (movement speed)
- MOUSE_SENS  (look sensitivity)
