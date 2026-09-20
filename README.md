# ImmersiveDialogue

A mod for **S.T.A.L.K.E.R. 2: Heart of Chornobyl** that lets you move, look
around and keep your walking animation during interactive NPC dialogue.

Vanilla STALKER 2 pins the camera on the NPC and locks you in place the moment
you press "talk". This mod restores agency: WASD / left stick walks, mouse /
right stick looks, the camera stays where you point it, and Skif's body
animates normally while he does it.

**Version 2.0 is a pure pak mod. It does not need UE4SS.** It is built with the
official S.T.A.L.K.E.R. 2 Zone Kit and installs like any other pak.

Nexus page: <https://www.nexusmods.com/stalker2heartofchornobyl/mods/2698>

Slava Ukraini.

## Features

- **Free movement in dialogue.** WASD (keyboard) and left stick (controller)
  walk Skif, camera-relative. Movement is available from the moment you press
  "talk", including during the camera zoom-in.
- **Free look.** Mouse and right stick look exactly as in normal play, using
  the game's own sensitivity, dead zones and invert settings. Works with any
  controller the game supports, including DualSense with native haptics.
- **Camera stays free.** The vanilla dialogue camera modifier that yanks your
  view onto the NPC is disabled while in dialogue.
- **Walking and strafing animations play in dialogue.** Vanilla freezes the
  animation inputs during dialogue; the mod feeds them from your actual
  movement.
- **Gestures don't fight the camera.** When Skif plays a dialogue gesture the
  view stays under your control and the legs run a straight-ahead cycle for
  the duration, so the gesture stays in frame.
- **Left stick doesn't scroll dialogue options.** The stick moves you; D-pad,
  arrow keys and mouse wheel still scroll answers. W and S no longer scroll
  answers either. F / Enter / click still confirms.
- Trading and inventory screens opened from a dialogue work as normal.

## Companion mod (recommended)

The dialogue FOV zoom-in is a config value (`DialogFOVDefault` in
`CoreVariables.cfg`) and is deliberately not touched by this mod, so it can't
conflict with other config mods. Install one of the Nexus "No Dialogue Zoom"
paks alongside, matching your normal FOV:
<https://www.nexusmods.com/stalker2heartofchornobyl/mods/71>,
<https://www.nexusmods.com/stalker2heartofchornobyl/mods/1499>,
<https://www.nexusmods.com/stalker2heartofchornobyl/mods/1933>.

## Requirements

- STALKER 2 (Steam or Epic install), current patch.
- Nothing else. No UE4SS.

## Install

### Vortex

1. Download `ImmersiveDialogue-v2.0.2.zip` from Nexus or this repo's Releases
   page and drop it into Vortex.
2. Vortex sees more than one pak in the zip and asks which files to install:
   tick the three `zzz_ImmersiveDialogue_20_P` files for the mod alone, or
   **Install All** to also get the optional add-on that hides the
   "press X to skip" prompt in dialogue (with free look, moving or looking
   around kept making it pop up; the skip key still works either way).
3. Deploy. Done. To change your choice later, reinstall from the zip.

### Manual

1. Get `ImmersiveDialogue-v2.0.2.zip` from the latest release.
2. Copy the three files from its `Main` folder
   ```
   zzz_ImmersiveDialogue_20_P.pak
   zzz_ImmersiveDialogue_20_P.ucas
   zzz_ImmersiveDialogue_20_P.utoc
   ```
   into `<GAME>\Stalker2\Content\Paks\~mods\` (create `~mods` if needed).
   Keep the three names identical apart from the extension.

   Optionally also copy the three `zzz_ImmersiveDialogueNoSkipHint_20_P.*`
   files from `Optional-NoSkipHint` to hide the dialogue skip prompt.

To uninstall, delete the files you copied.

## Known limitations

- **No arms in dialogue.** Look down while walking in a dialogue and Skif's
  arms are not there; they only appear while he gestures. Cosmetic; planned.
- **Camera centering cannot be toggled** in 2.0 (the 1.x DLL had an F6 toggle
  and a config file). It is always off in dialogue.
- The walking pace in dialogue is a fixed fraction of walk speed (no config).

## Compatibility

- The mod overrides three game assets: `AnimBP_Player` (player animation
  Blueprint), `BP_Stalker2Character` (player pawn Blueprint) and `IMC_Dialog`
  (dialogue input mapping). No config files are touched. Any other mod that
  overrides one of those will be overridden by this mod, because the pak is
  named to load last (`_20_P`). Weapon-positioning mods that ship their own
  `AnimBP_Player` are the likely conflict.
- **Known conflict: ZoneWatch (ZST watch mod).** It also overrides
  `AnimBP_Player`, so with both installed the watch check does nothing. A
  compatibility build is being worked out with its author. Until then it's one
  or the other.
- **Immersive HUD:** compatible (tested with 2.0.2). Its compass / HUD keys keep
  working before and after conversations.

- **Zone Standard Time (ZST watch mod).** ZST ships its own `AnimBP_Player`
  that includes this mod's walk wiring and loads above ours, so the watch works
  with both installed. ZST 1.0.5 only recognises the 2.0.2 build of this mod;
  with 2.0.3 or later the watch and the dialogue camera work, but Skif's legs
  don't animate while walking in dialogue until ZST updates. The UE4SS version
  (1.x) has no conflict with ZST at all.
- Mod authors: see [Compatibility for mod authors](#compatibility-for-mod-authors).
- Every game patch requires this mod to be rebuilt against the new Zone Kit.
  If a patch breaks it, check the Nexus page for an update.
- Save games are unaffected; the pawn class keeps its vanilla path.

## Compatibility for mod authors

Immersive Dialogue replaces three game assets. Unreal loads exactly one copy of
an asset, so if your mod replaces one of them too, whichever pak loads last wins
and the other mod's edits to that asset are gone. Load order cannot merge them;
one file has to carry both sets of edits.

| Asset | What we change | Status |
|---|---|---|
| `/Game/_STALKER2/Animations/Player/AnimBP_Player` | the dialogue walk wiring (below) | **frozen from 2.0.4**: we do not plan to change it again |
| `/Game/GameLite/Blueprints/Characters/Player/BP_Stalker2Character` | movement in dialogue, camera handling, cutscene guard | changes between releases; going away in 2.1 (moves to a mod-only helper) |
| `/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Dialog` | move / look rows added, W / S / left-stick "select answer" rows removed | going away in 2.1 (own mapping context) |

Our pak is named `zzz_ImmersiveDialogue_20_P` (load order 2103). A pak that
carries a combined copy of one of these assets must load above it, e.g. `_30_P`.

### Merging our `AnimBP_Player` block into yours

From 2.0.4 our part of `AnimBP_Player` is only this, and it is meant to be merged
once and left alone:

- **Variables:** `DlgMoving` (bool), `DlgFwd`, `DlgRight`, `LastInputTime` (float).
- **Event Graph**, off `Event Blueprint Update Animation`: `Try Get Pawn Owner` ->
  `Cast To PC` -> `Is In Static Dialog` -> Branch. In dialogue: read the pawn's
  `Movement Input Vector`; if its length is above 0.01, store the game time in
  `LastInputTime` and the normalized X / Y times 0.86 in `DlgFwd` / `DlgRight`;
  `DlgMoving` = game time minus `LastInputTime` is under 0.15; while
  `Is Any Montage Playing`, `DlgRight` = 0 and (if moving) `DlgFwd` = 0.86.
  Not in dialogue: all three reset.
- **AnimGraph**, Moving state machine: `DlgMoving` OR'd / AND-NOT'd into the
  Idle, IsMoving, Walk and StopWalk transition rules, and the Walk / StartWalk
  blendspace X / Y inputs switched to `DlgRight` / `DlgFwd` while `DlgMoving`.

Node-by-node detail is in [BUILD.md §5.4](BUILD.md); the source asset is
`zonekit/ImmersiveDialogue/Content/_STALKER2/Animations/Player/AnimBP_Player.uasset`.

The block needs **no detection gate**. It only reacts to `Movement Input Vector`
during static dialogue, and nothing writes that in dialogue except our player
character, so without Immersive Dialogue installed it does nothing. It contains
no camera code (that moved to the player character in 2.0.4; if you merged an
earlier version, remove `CamAbs`, `SavedCamRot`, `SavedOrient` and the
`Set Absolute` / `Set World Rotation` nodes that came with it).

### Detecting Immersive Dialogue

If you want to know whether we are installed, load this soft object path:

```
/Game/ImmersiveDialogueCompat/ID_AnimInterface_v1.ID_AnimInterface_v1
```

It is an empty curve asset shipped in the main pak from 2.0.4, and its name is
an **interface revision, not a mod version**: `v1` means our player character is
present, feeds `Movement Input Vector` during dialogue and owns the dialogue
camera, and the animation block is the one described above. It stays `v1` across
releases for as long as that holds.

Do **not** detect us through `/Game/__ModKitWwiseCookAnchor_ImmersiveDialogue_<number>__`.
The Zone Kit generates that asset on every cook with a new number, so it matches
exactly one build.

The UE4SS version (1.x) replaces no assets and needs none of this.

Everything here is MIT-licensed; merge what you need. Questions and patches:
GitHub issues, or the Nexus page (mod 2698).

## Versions

- **2.0.4** — compatibility release; dialogue feels the same. Camera handling
  moved from the player animation file into the player character, so it keeps
  working when another mod's copy of the animation file is in use. Our part of
  `AnimBP_Player` is now only the walk wiring and is frozen. New stable marker
  asset for other mods to detect this one.
- **2.0.3** — the mod now stands down during cutscenes. Story scenes that run
  through the dialogue system (the prologue's dog and Richter scenes, "Back to
  the Slag Heap", the DLC's "Distant Mirage") could end up with the camera stuck
  facing one way or unable to tilt up and down. The mod only acts when the game
  is not in a cinematic and has not taken look control away.
- **2.0.2** — walk animation in dialogue now follows your input instead of the
  body's velocity: reversing direction (A to D) no longer stops and restarts
  the walk, and the walk-start delay on a cold start is gone. The zip now also
  carries an optional add-on pak that hides the dialogue skip prompt.
- **2.0.1** — fixed keys (Q, E, L, middle mouse; pad X, Y, D-pad) going dead
  after a dialogue until a save reload.
- **2.0.0** — rewrite as a Zone Kit pak. No UE4SS. Everything the 1.x DLL
  did, plus proper walk/strafe animation in dialogue and controller support
  through the game's own input system.
- **1.0.x** — UE4SS C++ DLL. Still available under the `v1.0.7` tag and
  earlier releases; source in `ImmersiveDialogueCpp/`. Not maintained.

## Build from source

See [BUILD.md](BUILD.md). The complete mod source (the Zone Kit plugin folder
with the edited assets) is in [zonekit/ImmersiveDialogue/](zonekit/ImmersiveDialogue/),
the tooling in [zonekit/tools/](zonekit/tools/), and the full engineering log
of how each piece was found is in [zonekit/README.md](zonekit/README.md).

## Credits

Made by Noah Semus with heavy pair-programming from Claude (Anthropic).
STALKER 2 is developed by GSC Game World in Kyiv, Ukraine.

## License

MIT — see [LICENSE](LICENSE).
