# Building ImmersiveDialogue 2.0 (Zone Kit pak)

This document is for anyone who wants to rebuild, modify or extend the mod. It
covers the toolchain, what the mod changes and exactly how, how to cook and
install it, and the traps we hit. The 1.x UE4SS DLL build is described at the
end for reference.

Everything below was done on Windows 11 with the Zone Kit that matches the
game's current patch. Paths assume the kit at `G:\Epic Games\STALKER2ZoneKit`
and the game at `C:\Program Files (x86)\Steam\steamapps\common\S.T.A.L.K.E.R. 2 Heart of Chornobyl`;
adjust in the scripts if yours differ.

## 1. Prerequisites

- **S.T.A.L.K.E.R. 2 Zone Kit** (GSC's Unreal 5.5 mod editor). Install it from
  the Epic Games Store. First launch compiles shaders and can take a long time.
- ~600 GB free where the kit lives (its content paks are huge).
- Git, and this repository cloned somewhere short-pathed.
- Optional, for the C++ diagnostic probe only: Visual Studio 2022 with the
  Desktop C++ workload, CMake 3.22+, Rust, and a clone of RE-UE4SS (see §7).

No Python installation is needed; the kit ships its own
(`Engine\Binaries\ThirdParty\Python3\Win64\python.exe`).

## 2. What the mod is

A Zone Kit "plain mod" plugin named `ImmersiveDialogue` that **overrides three
game assets** by placing edited copies at the same relative path under the mod's
`Content` folder. The kit cooks them into an "OverrideContent" pak; at runtime
they replace the originals. A second, optional plugin `ImmersiveDialogueNoSkipHint`
overrides one more asset (§5.6) and cooks to its own pak, shipped in the same zip under `Optional-NoSkipHint/` so users can opt in (Vortex shows a file chooser when a zip holds more than one pak set).

| File (under `Content/`) | Kind | What the edit does |
|---|---|---|
| `_Stalker_2/data/input/InputMappingContexts/IMC_Dialog.uasset` | Input Mapping Context | removes the W / S / left-stick "select answer" rows; adds the move (`IA_LocomotionForward`) and look (`IA_LookUp`) rows copied from `IMC_Exploration` |
| `GameLite/Blueprints/Characters/Player/BP_Stalker2Character.uasset` | player pawn Blueprint | handles the move action itself in dialogue; per tick in dialogue disables the LookAt camera modifier and makes sure `IMC_Dialog` is active |
| `_STALKER2/Animations/Player/AnimBP_Player.uasset` | player animation Blueprint | feeds walk/strafe from real movement while the game's own inputs are frozen; camera decouple; gesture handling |

Why each piece exists (verified with an in-game probe):

- The game's native player class drops the **move** action while
  `IsInStaticDialog()` is true, but not the **look** action. So look only needs
  the mapping context; move needs the pawn Blueprint to add movement itself.
- `IMC_Dialog` only becomes active when the dialogue UI opens, so the pawn adds
  it as soon as `IsInStaticDialog()` is true (gives control during the zoom-in).
- In dialogue the anim instance's native update keeps running but leaves
  `StateData.bMoving`, `StateData.bWalking` and
  `LocomotionData.MovementPlayRate.{Right,Forward}` frozen at 0 and sets
  `bWalkingOverride = 1`. Those struct members are not Blueprint-writable, so the
  anim Blueprint computes its own values and the graph's bindings are rerouted
  to them while in dialogue.
- Player dialogue gestures are montages on the main anim instance
  (`IsAnyMontagePlaying()` is true while one plays).

The complete mod source, including the binary `.uasset` files with all edits
applied, is committed in [`zonekit/ImmersiveDialogue/`](zonekit/ImmersiveDialogue/).
**The fastest way to build is to use those files as-is (§4).** §5 documents
every edit so they can be reproduced or changed.

## 3. Repository layout

```
zonekit/
  ImmersiveDialogue/          the Zone Kit mod plugin (copy into <kit>\Stalker2\Mods\)
    ImmersiveDialogue.uplugin
    Content/                  the three overrides + the plugin's GameFeature data asset
  ImmersiveDialogueNoSkipHint/  optional second plugin: W_SkipHintView override (§5.6)
  tools/
    cook_and_install.ps1      cook + install in one go; -Mod <plugin name> (default ImmersiveDialogue)
    install_paktest.ps1       install the last cooked pak as the _30_P dev test pak; -Mod
    revert_paktest.ps1        remove the dev test paks
    make_imc_override.py      regenerates IMC_Dialog.uasset headlessly (§5.2)
    dump_imc.py               dumps any mapping context to JSON
    extract_from_pak.py       pulls uncooked assets out of the kit's editor pak
    dump_names.py             string dump of a .uasset (find what it references)
    ue_exec.py                run editor Python via remote execution (if enabled)
    probe/main.lua            UE4SS Lua probe (limited; see §7)
    classifier/<Mod>/OverridePackages.txt, NewPackages.txt   package classifier lists the cook reads
  builds/                     cooked paks of each checkpoint and release
  README.md                   engineering log: how every mechanism was found
ImmDlgProbeCpp/               UE4SS C++ diagnostic probe (§7), not part of the mod
ImmersiveDialogueCpp/         the 1.x UE4SS DLL (legacy, §8)
```

## 4. Build the mod from the committed source

1. Copy `zonekit\ImmersiveDialogue\` to `<kit>\Stalker2\Mods\ImmersiveDialogue\`
   (and `zonekit\ImmersiveDialogueNoSkipHint\` next to it if you want that pak).
2. Copy `zonekit\tools\classifier\<Mod>\OverridePackages.txt` and `NewPackages.txt` to
   `<kit>\Stalker2\SavedMods\PackageClassifier\<Mod>\` (create the
   folder). The cook reads these lists to know which packages are overrides.
   The editor may append a stale `Autogenerated_*_ActorReplacementData` line to
   the kit's copy; harmless (no such asset exists, nothing is cooked for it).
3. Cook and install:
   ```
   powershell -File zonekit\tools\cook_and_install.ps1
   ```
   This runs the same UAT command the editor's *Package Mod* button runs:
   ```
   <kit>\Engine\Build\BatchFiles\RunUAT.bat GSCCookMod
       "-Project=<kit>\Stalker2\Stalker2.uproject"
       "-PluginPath=<kit>\Stalker2\Mods\ImmersiveDialogue\ImmersiveDialogue.uplugin"
       "-PackageClassifierOutputDir=<kit>\Stalker2\SavedMods\PackageClassifier\ImmersiveDialogue"
       "-UnrealExe=<kit>\Stalker2\Binaries\Win64\Stalker2ModEditor-Win64-Shipping-Cmd.exe"
       -TargetPlatform=Win64 -nocompile -nocompileuat
   ```
   (`-UnrealExe` is required; without it UAT looks for a stock `UnrealEditor-Cmd.exe`.)
   It takes 5-6 minutes. Output:
   `<kit>\Stalker2\SavedMods\Staged\ImmersiveDialogue\Windows\OverrideContent\Windows\Stalker2\Mods\ImmersiveDialogue\Content\Paks\Windows\ImmersiveDialogueStalker2-Windows-OverrideContent.{pak,ucas,utoc}`.
4. The install step copies those three files to
   `<game>\Stalker2\Content\Paks\~mods\zzz_ImmersiveDialogue_PakTest\` renamed
   `zzz_ImmersiveDialogue_20_P.{pak,ucas,utoc}`. **The rename matters**: Zone
   Kit paks mount at priority 3; many Nexus mods use `_P` (103) or `_10_P`
   (1103) names and one of them may ship its own `AnimBP_Player`. `_20_P`
   mounts at 2103, above them. Without it the anim override silently loses and
   nothing animates. (Priority = 3 + 100 × (N+1) for a `_N_P` suffix.)

When a released copy of this mod is also installed through Vortex (it ships as
`_20_P`), the dev test copy is named `zzz_ImmersiveDialogue_30_P` instead
(order 3103) so the build under test wins unambiguously; `install_paktest.ps1`
does this. Release zips keep the `_20_P` name.

The kit's *Package Mod* toolbar button does the same cook; if you use it, copy
and rename the output by hand. Note it also generates an
`Autogenerated_*_ActorReplacementData.uasset` for the pawn override; the mod
works without it (plain path override) and we ship without it; delete it from
the mod's `Content` folder if the editor creates one, or it gets cooked in.

You do not need the editor open to cook. If it is open, that's fine too.

## 5. The edits, asset by asset

### 5.1 Config files

None. The dialogue FOV zoom lives in `CoreVariables.cfg` (`DialogFOVDefault`)
and is left to the existing Nexus "No Dialogue Zoom" paks so this mod never
clashes with other config overrides. (A full-file config override would
silently replace every other mod's edits to that file.)

### 5.2 IMC_Dialog (generated by script)

`zonekit\tools\make_imc_override.py` is a Python script that runs **inside the
editor** as a commandlet (about 4.5 minutes, editor need not be open):

```
<kit>\Stalker2\Binaries\Win64\Stalker2ModEditor-Win64-Shipping-Cmd.exe "<kit>\Stalker2\Stalker2.uproject" -run=pythonscript -script=<repo>\zonekit\tools\make_imc_override.py -unattended -nosplash -stdout -NoShaderCompile
```

(Edit the `LOG` path at the top first.) It:

1. Duplicates `/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Dialog` to
   `/ImmersiveDialogue/_Stalker_2/data/input/InputMappingContexts/IMC_Dialog`.
2. Removes the four `IA_UI_Dialog_SelectAnswer` rows bound to `W`, `S`,
   `Gamepad_LeftStick_Up`, `Gamepad_LeftStick_Down`. (Up / Down arrows, D-pad
   and mouse wheel rows stay.)
3. Duplicates `IMC_Exploration` to a temporary asset and **moves** its modifier
   and trigger objects (`rename(outer=…)`) into the override for every
   `IA_LocomotionForward` and `IA_LookUp` row (W, A, S, D, Gamepad_Left2D;
   Mouse2D, Gamepad_Right2D, NumPad 1/2/3/5). Moving the real objects keeps the
   game's dead zones, response curves and the custom `ApplySensitivity`
   modifier intact. (Creating new modifier objects and setting their properties
   fails with "cannot be edited on templates"; do not try.)
4. Saves. Do **not** call `delete_asset` on the emptied temp asset; it crashes
   the commandlet. The temp asset is never saved, so nothing is left behind.

The result has 34 rows. `dump_imc.py` prints any context's rows for checking.

### 5.3 BP_Stalker2Character (player pawn)

In the editor: Content Browser → `/Game/GameLite/Blueprints/Characters/Player/BP_Stalker2Character`
→ right-click → **Checkout selected content to mod plugin folder**. Open the
mod's copy; the vanilla Event Graph only has a jump-force block and a debug
teleport key. Add:

**Movement handler**

```
EnhancedInputAction IA_LocomotionForward
  Triggered ─► Branch (Condition: Is In Static Dialog)
                 True ─► Add Movement Input (World Direction = Forward Vector, Scale = X × 0.35)
                      ─► Add Movement Input (World Direction = Right Vector,   Scale = Y × 0.35)
                      ─► Set Move Vector (Make Vector (X, Y, 0))
  Completed ─► Set Move Vector (Make Vector (0, 0, 0))
```
where `X`, `Y` = Break Vector2D of the event's Action Value (X forward, Y right),
and Forward/Right Vector come from `Make Rotator (0, ControlRotation.Yaw, 0)`.
`Set Move Vector` is the game's own input feed (native `PC` function); the
`Completed` call stops the character when the key is released.

**Per-tick dialogue upkeep**

```
Event Tick ─► Branch (Is In Static Dialog)
  True ─► Cast To PlayerController (Object = Get Controller)
        ─► Find Camera Modifier By Class (Player Camera Manager, class CameraModifier_LookAt)
        ─► Is Valid
              Is Valid     ─► Disable Modifier (Immediate) ─► Remove Camera Modifier ─┐
              Is Not Valid ───────────────────────────────────────────────────────────┤
        ┌──────────────────────────────────────────────────────────────────────────────┘
        └► Branch (Has Mapping Context (IMC_Dialog))      [subsystem = Get EnhancedInputLocalPlayerSubsystem]
              False ─► Add Mapping Context (IMC_Dialog, Priority 1)
```
Pick the `/Game/…/IMC_Dialog` asset in those pins (not the `/ImmersiveDialogue/`
one); at runtime that path resolves to the override.

**Dialogue-exit cleanup (2.0.1)**

The `Add Mapping Context` above runs on every tick where `IsInStaticDialog()` is
true, which includes the ticks *after* the dialogue UI has already removed
`IMC_Dialog` on its way out (closing a trade screen, ending the conversation).
The context gets re-added and nobody removes it, so its rows keep consuming
`Q`, `E`, `F`, `L`, the pad face buttons, D-pad up/down and mouse wheel in normal
play until a save is loaded. Reported on Nexus 2026-09-14 as "quick slots Q/E
stop working after talking to NPCs" (Zenzi0) and "can't open the backpack on a
PS5 pad after a trade" (Saigaiii866). Fix: remember that *we* added it and take
it back out the first tick after dialogue ends.

The committed asset has this. A Boolean variable `DlgImcAdded` (default false) and the tick chain is:

```
Event Tick ─► Branch (Is In Static Dialog)
  True  ─► … (LookAt modifier removal, unchanged) …
        ─► Branch (Has Mapping Context (IMC_Dialog))
              False ─► Add Mapping Context (IMC_Dialog, Priority 1) ─► Set DlgImcAdded = true
  False ─► Branch (DlgImcAdded)
              True  ─► Remove Mapping Context (IMC_Dialog) ─► Set DlgImcAdded = false
```

`Remove Mapping Context` is on the same `EnhancedInputLocalPlayerSubsystem`
node (drag from its data pin). Removing a context that the game already removed
is a no-op, so the order of the game's own removal versus ours does not matter.
Keep `Add`/`Remove` on the same asset pin (`/Game/…/IMC_Dialog`).

This cannot affect trading: the trade screen opens *inside* the dialogue, where
`IsInStaticDialog()` is still true, so the False branch (and the removal) only
runs once the whole conversation is over.

Trading itself needs no change: in 2.0.0 the player does not walk while the
trade screen is open (confirmed in play; the screen takes UI-only input), and
this edit does nothing until the conversation is over.

**Cinematic guard (2.0.3)**

Story cutscenes that run through the dialogue system report `IsInStaticDialog()`
as true, so the mod treated them as conversations and took the camera over.
The pawn now has a **pure function `ImmDlgActive`** (one Boolean output `Active`):

```
Active = Is In Static Dialog
         AND NOT ( Is In Cinematic
                   OR  Get Controller -> Cast To PlayerController (pure) -> Is Look Input Ignored )
```

`Is In Cinematic` is on the pawn's native base class (`Obj`). Both Branches that
used `Is In Static Dialog` directly (the one after `IA_LocomotionForward` ->
Triggered and the one after `Event Tick`) take `ImmDlgActive` as their Condition
instead. When the guard turns false mid-scene the Tick's False path runs, so the
mapping context we added is removed as on a normal dialogue exit.

**Dialogue camera (2.0.4, moved here from the anim Blueprint)**

Variables: `CamAbs` (Boolean), `SavedCamRot` (Rotator), `SavedOrient` (Boolean),
`CamRestoreUntil` (Float). "Camera" below is the node `Get Camera Component`
(Target is PC, self); "Character Movement" is the component getter.

- Comment box **`Cam: init`**: `Event BeginPlay` -> `SET SavedCamRot` (camera ->
  `Get Relative Rotation`) -> `SET SavedOrient` (Character Movement ->
  `Get Orient Rotation to Movement`). A clean value always exists before the
  first dialogue.
- Comment box **`Cam: in dialogue`** (every tick while `ImmDlgActive`): Branch
  `cam clean?` = NOT camera -> `Get Absolute Rotation`; True -> re-capture
  `SavedCamRot` and `SavedOrient`. Then, from both Branch outputs: camera ->
  `Set Absolute` (only `New Absolute Rotation` ticked) -> camera ->
  `Set World Rotation` (`Get Control Rotation`) -> `SET CamAbs` true ->
  Character Movement -> `Set Orient Rotation to Movement` false.
- Comment box **`Cam: after dialogue`** (every tick while not `ImmDlgActive`), a
  `Sequence` labelled `seq: cam after`: `Then 0` -> Branch `was abs?` (`CamAbs`)
  True -> `SET CamAbs` false -> `SET CamRestoreUntil` = `Get Game Time in
  Seconds` + 0.5 -> `Set Orient Rotation to Movement` (`SavedOrient`). `Then 1`
  -> Branch `restore window?` (`Get Game Time in Seconds` < `CamRestoreUntil`)
  True -> camera -> `Set Absolute` (nothing ticked) -> camera ->
  `Set Relative Rotation` (`SavedCamRot`).
- Hook-up: the `Event Tick` Branch (`ImmDlgActive`) True -> `Sequence`
  `seq: in dialogue` (`Then 0` -> `cam clean?`, `Then 1` -> the existing
  `Cast To PlayerController`); False -> `Sequence` `seq: not in dialogue`
  (`Then 0` -> the existing `Branch (Dlg Imc Added)`, `Then 1` -> `seq: cam after`).

The half-second restore window and the clean-only capture exist because another
mod's `AnimBP_Player` may still contain our pre-2.0.4 camera code and run it in
the same frames; this makes the pawn's result the one that sticks. `Sequence`
nodes keep the chains from looping or dead-ending.

**Compatibility marker (2.0.4).** `ImmersiveDialogueCompat/ID_AnimInterface_v1`
in the mod's Content (an empty CurveFloat made by `zonekit/tools/create_marker.py`)
is listed in `OverridePackages.txt` and cooks to
`/Game/ImmersiveDialogueCompat/ID_AnimInterface_v1`. Other mods load that path
to detect us. Never rename or remove it; see the README's mod-author section.

### 5.4 AnimBP_Player (player animation Blueprint)

Checkout `/Game/_STALKER2/Animations/Player/AnimBP_Player` the same way.

**Frozen from 2.0.4.** Other mods merge this block into their own
`AnimBP_Player` (ZST does), so it must not change; put new logic in the pawn.

**Variables:** `DlgMoving` (Boolean), `DlgFwd` (Float), `DlgRight` (Float),
`LastInputTime` (Float), `DbgState` (String, diagnostic only).

**Event Graph** (was empty). One chain off `Event Blueprint Update Animation`:

```
Try Get Pawn Owner ─► Cast To PC ─► Is In Static Dialog ─► Branch
```

The Branch (comment "In dialogue?") takes `Is In Static Dialog` directly. 2.0.3
had the cinematic guard inline here; 2.0.4 removed it again because the pawn does
not write `Movement Input Vector` during cinematics, so this block stays quiet
there on its own.

*True (in dialogue), in order:*
1. `Get Movement Input Vector` (the pawn property that `Set Move Vector` writes;
   X forward, Y right, control space) → `Vector Length` → `> 0.01` → Branch:
   - True → **Set LastInputTime** = `Get Game Time in Seconds` → `Normalize`
     (same vector) → `Break Vector` → X × 0.86 → **Set DlgFwd**; Y × 0.86 →
     **Set DlgRight**.
   - False → nothing (the last direction is kept).
2. **Set DlgMoving** = `(Get Game Time in Seconds − LastInputTime) < 0.15`.

   Input, not velocity, drives this (changed after 2.0.1). Velocity dips through
   zero when the player flips A→D, which fired Walk→StopWalk and restarted the
   walk from Idle with a visible walk-start; it also lagged on a cold start while
   the body accelerates. The 0.15 s hold covers the frame or two between a key
   release and the next key press. Confirmed in-game: A↔D flips reverse without
   stopping and the cold-start walk-start is gone.
3. Branch (`Is Any Montage Playing`): True → **Set DlgRight = 0** → Branch
   (DlgMoving) True → **Set DlgFwd = 0.86**. (Gesture playing: legs go straight
   ahead so the torso never twists under the gesture.)

(Up to 2.0.3 steps 4-5 here captured and took over the camera; that lives in the
pawn now, see "Dialogue camera" in §5.3.)

*False (not in dialogue):* **Set DlgMoving = false, DlgFwd = 0, DlgRight = 0**.

*Diagnostic events (safe to delete):* `AnimNotify_EnterIdle`, `…EnterStartWalk`,
`…EnterWalk`, `…EnterStopWalk`, `…EnterStartRun`, `…EnterRun`, `…EnterJog`,
`…EnterLowCrouch`, each → Set DbgState "<name>". They are fired by *Entered
State Event* names set on the Moving state machine's states.

**AnimGraph → Locomotion → Moving state machine.** Open each transition's rule
(click the round icon on the arrow):

| Transition | Rule |
|---|---|
| Idle → IsMoving | `StateData.bMoving OR DlgMoving` |
| IsMoving → StartWalk | `(NOT StateData.bWalkingOverride) OR DlgMoving` |
| IsMoving → StartRun, IsMoving → LowCrouch (any other exit of IsMoving) | original `AND NOT DlgMoving` |
| Walk → StopWalk | `NOT (StateData.bMoving OR DlgMoving)` |
| Walk → anything else (StartRun/Run/Jogging/LowCrouch, whichever exist) | original `AND NOT DlgMoving` |

`DlgMoving` is a plain `Get` of the variable; the `StateData…` values are the
pins already present on the rule's getter node. Rules that use a Blueprint
node instead of a direct binding show a harmless "uses Blueprint to update its
values" warning.

**Walk state and StartWalk state.** Each contains several `Blendspace Player`
nodes whose **X** and **Y** pins are bound to
`LocomotionData.MovementPlayRate.RightValue` / `.ForwardValue`. For every
player: click the pin's binding dropdown → *Remove Binding*, then wire:

```
X ◄── Select Float (Pick A = DlgMoving; A = DlgRight; B = Property Access LocomotionData.MovementPlayRate.RightValue)
Y ◄── Select Float (Pick A = DlgMoving; A = DlgFwd;   B = Property Access LocomotionData.MovementPlayRate.ForwardValue)
```
One pair of Select nodes can feed all players in a state. Leave **Play Rate**
and **Blend Space** bound. (Property Access nodes are fine inside the
AnimGraph; see §6 about the event graph.)

**Top-level AnimGraph, "Additional Pose" group:** the dead-body-drag
`Blend Poses by bool` has its Active Value changed from `StateData.bMoving` to
`StateData.bMoving OR DlgMoving`. Harmless either way; it can be reverted.

**Do not override `AnimBP_player_bh`** (the bare-hands weapon layer). A recook
of that asset breaks the unarmed sprint left-hand animation from a fresh load.
It is not needed.

### 5.5 Tunables

| What | Where | Value |
|---|---|---|
| Walk speed in dialogue | pawn BP, both multipliers | 0.35 |
| Direction strength fed to the blendspaces | anim BP, both multipliers and the gesture forward value | 0.86 |
| Movement detection threshold | anim BP, `> 10` on velocity length | 10 cm/s |

### 5.6 Optional pak: ImmersiveDialogueNoSkipHint (W_SkipHintView)

The dialogue's "press X to skip" prompt is its own widget,
`/Game/GameLite/FPS_Game/UIRemaster/Dialogue/W_SkipHintView`, whose show/hide
logic is native (`SkipHintView` C++ base; the Blueprint only holds the layout and
the fade animation). With free look, any move/look input makes it pop up, so the
companion pak simply hides it. Separate plugin so users can opt in.

The plugin was created with `<kit>\CreatePlainMod.bat ImmersiveDialogueNoSkipHint`
and its `.uplugin` then given the same descriptor fields as the main mod (Game
Features category, `Mod: true`, `ExplicitlyLoaded`). The editor discovers new
plugins only at startup and generates the plugin's GameFeatureData asset on the
first launch after that.

Edit: with the kit's active-mod selector set to `ImmersiveDialogueNoSkipHint`,
check out `W_SkipHintView` to the mod, open it, Designer tab, set **Visibility =
Collapsed** on the root panel and on `SkipContainer`. Compile, save. Cook with
`cook_and_install.ps1 -Mod ImmersiveDialogueNoSkipHint`. Installed name:
`zzz_ImmersiveDialogueNoSkipHint_20_P.*`. The skip key itself still works; only
the hint is gone.

## 6. Editor and pipeline traps

- **The editor crashes when it auto-reopens `AnimBP_Player` at startup**
  (compile-on-load before the mod's classes exist). Open it by hand after
  launch instead. If the crash loop starts, delete the `OpenAssetsAtExit=` lines
  from `%LOCALAPPDATA%\Stalker2\Saved\Config\WindowsEditor\EditorPerProjectUserSettings.ini`
  with the editor closed.
- **Opening `BP_Stalker2Character` crashed while the modified `AnimBP_Player`
  was in the mod folder** (not reproduced since 2026-09-17; for 2.0.3 both were
  edited in one session. If it comes back:) (the pawn editor previews the anim class). To edit the
  pawn, move `AnimBP_Player.uasset` out of the mod folder temporarily, edit,
  then move it back.
- **Property Access nodes in the anim Blueprint's Event Graph** crashed the
  compiler once. Use them only inside the AnimGraph / transition rules.
- **Save before Compile** on big edits; a compiler crash loses unsaved work.
- The cook picks up **everything** in the mod's `Content` folder regardless of
  the classifier lists; keep only the files you mean to ship there.
- The **mod folder is the source of truth** for the editor. After editing, copy
  the changed `.uasset` back into `zonekit/ImmersiveDialogue/` in the repo.
- Blueprint "Float" variables are doubles; read them as such from C++.

## 7. Diagnostics

Two probes exist for when something "doesn't animate" and you need to know
what the anim instance is actually doing. Both need UE4SS installed
(`ImmDlgProbeCpp : 1` in `ue4ss\Mods\mods.txt`, **listed before
`UObjectCacheMod`** or object lookups return stale objects).

- `ImmDlgProbeCpp/` — C++ UE4SS mod. Build with the root `CMakeLists.txt`
  (RE-UE4SS clone required, see §8) and copy
  `Output\ImmDlgProbeCpp\Game__Shipping__Win64\main.dll` to
  `ue4ss\Mods\ImmDlgProbeCpp\dlls\main.dll`. While in dialogue it logs, twice a
  second (every frame for 1.5 s after movement starts), the anim class, our
  variables, the `DbgState` string, linked layer classes, montage/slot state and
  the frozen `StateData` / `LocomotionData` values to `ue4ss\UE4SS.log`. All
  engine calls are SEH-guarded; it reads only while `IsInStaticDialog()`.
  Calling `GetCurrentStateName` from outside the anim update faults; that is
  why the anim Blueprint reports the state through `DbgState` instead.
- `zonekit/tools/probe/main.lua` — Lua version. Property reads only; UE4SS Lua
  cannot read struct members and calling anim functions from it crashes.

`zonekit/README.md` has the full log of what each probe run showed.

## 8. Legacy: the 1.x UE4SS DLL

Source in `ImmersiveDialogueCpp/dllmain.cpp`. Build: Visual Studio 2022 with
Desktop C++, CMake 3.22+, Rust (for RE-UE4SS's `patternsleuth`), and a
recursive clone of <https://github.com/UE4SS-RE/RE-UE4SS> into `RE-UE4SS/`
(the `UEPseudo` submodule requires a GitHub account linked to Epic Games; set
`git config --global url."https://github.com/".insteadOf "git@github.com:"`).

```
cmake -S . -B Output
cmake --build Output --config Game__Shipping__Win64 --target ImmersiveDialogue
```
Only `Game__Shipping__Win64` matches the game's CRT. Output
`Output\ImmersiveDialogueCpp\Game__Shipping__Win64\main.dll` installs to
`ue4ss\Mods\ImmersiveDialogueCpp\dlls\main.dll` with `ImmersiveDialogueCpp : 1`
in `mods.txt`. Do not run the DLL and the pak together.
