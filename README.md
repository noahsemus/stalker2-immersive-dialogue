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

- **Free movement in dialogue.** Your movement keys (keyboard) and left stick
  (controller) walk Skif, camera-relative. Movement is available from the
  moment you press "talk", including during the camera zoom-in. The keys follow
  your bindings in Options > Controls, so AZERTY (ZQSD) and other layouts work.
- **Free look.** Mouse and right stick look exactly as in normal play, using
  the game's own sensitivity, dead zones and invert settings. Works with any
  controller the game supports, including DualSense with native haptics.
- **Camera stays free.** The vanilla dialogue camera modifier that yanks your
  view onto the NPC is disabled while in dialogue.
- **Walking and strafing animations play in dialogue**, with the same body turn
  as outside a conversation. Vanilla freezes the animation inputs during
  dialogue; the mod animates the body from your actual movement.
- **Arms in dialogue.** Look down and Skif's arms are there, and his dialogue
  gestures play on them.
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

1. Download `ImmersiveDialogue-v2.1.0.zip` from Nexus or this repo's Releases
   page and drop it into Vortex.
2. Vortex sees more than one pak in the zip and asks which files to install:
   tick the six files of the mod (`zzz_ImmersiveDialogue_20_P.*` and
   `ImmersiveDialogueStalker2-Windows-NewContent.*`) for the mod alone, or
   **Install All** to also get the optional add-on that hides the
   "press X to skip" prompt in dialogue (with free look, moving or looking
   around kept making it pop up; the skip key still works either way).
3. Deploy. Done. To change your choice later, reinstall from the zip.

### Manual

1. Get `ImmersiveDialogue-v2.1.0.zip` from the latest release.
2. Copy the six files from its `Main` folder
   ```
   zzz_ImmersiveDialogue_20_P.pak
   zzz_ImmersiveDialogue_20_P.ucas
   zzz_ImmersiveDialogue_20_P.utoc
   ImmersiveDialogueStalker2-Windows-NewContent.pak
   ImmersiveDialogueStalker2-Windows-NewContent.ucas
   ImmersiveDialogueStalker2-Windows-NewContent.utoc
   ```
   into `<GAME>\Stalker2\Content\Paks\~mods\` (create `~mods` if needed).
   Don't rename them.

   Optionally also copy the three `zzz_ImmersiveDialogueNoSkipHint_20_P.*`
   files from `Optional-NoSkipHint` to hide the dialogue skip prompt.

To uninstall, delete the files you copied.

## Known limitations

- **Camera centering cannot be toggled** in 2.0 (the 1.x DLL had an F6 toggle
  and a config file). It is always off in dialogue.
- The walking pace in dialogue is a fixed fraction of walk speed (no config).
- **Rebinding movement:** the game resolves a clash inside the dialogue screen
  by unbinding the other action. Rebinding "move left" to Q (AZERTY) leaves the
  dialogue's "open upgrade" prompt without a key until you give it another one
  in Options > Controls.

## Compatibility

- **From 2.1.0 the mod no longer replaces the player animation file
  (`AnimBP_Player`).** Watch, weapon and animation mods that ship their own
  copy of it work alongside this one with no patch and no load-order rule. In a
  conversation this mod animates Skif's body itself; outside conversations the
  game's (or your other mod's) animation runs untouched.
- The mod still overrides two game assets: `BP_Stalker2Character` (player pawn
  Blueprint) and `IMC_Dialog` (dialogue input mapping). No config files are
  touched. A mod that overrides one of those will be overridden by this one
  (the pak is named to load late, `_20_P`).
- **Immersive HUD:** compatible (tested with 2.0.2). Its compass / HUD keys keep
  working before and after conversations.
- **Zone Standard Time (ZST) and other watch mods:** compatible from 2.1.0 with
  no patch (tested with ZST). The watch works as usual; during a conversation
  Skif's body is animated by this mod.
- Mod authors: see [Compatibility for mod authors](#compatibility-for-mod-authors).
- Every game patch requires this mod to be rebuilt against the new Zone Kit.
  If a patch breaks it, check the Nexus page for an update.
- Save games are unaffected; the pawn class keeps its vanilla path.
- **Keybinds:** the dialogue movement rows carry the game's own "Move
  Forward / Back / Left / Right" names, so the game applies your Options >
  Controls bindings to them.

## Compatibility for mod authors

From 2.1.0 Immersive Dialogue does **not** replace `AnimBP_Player`. It replaces:

| Asset | What we change | Status |
|---|---|---|
| `/Game/GameLite/Blueprints/Characters/Player/BP_Stalker2Character` | movement in dialogue, camera handling, cutscene guard | changes between releases; planned to move to the mod-only helper |
| `/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Dialog` | move / look rows added (with the game's mappable names), W / S / left-stick "select answer" rows removed | planned to become our own context |

Our pak is named `zzz_ImmersiveDialogue_20_P` (load order 2103).

**How the dialogue body works now.** A mod-only world subsystem attaches our own
animation Blueprint to the player mesh as its **post-process anim instance**
(`SetOverridePostProcessAnimBP`) and enables it only while the player is in a
static dialogue and not in a cinematic. It runs a copy of the player graph fed
with the main instance's data every frame, so whatever `AnimBP_Player` is loaded
(vanilla or yours) stays untouched and runs everywhere else. The one new contact
point: if your mod sets its own post-process anim Blueprint on the player mesh,
ours replaces it during dialogue.

**If you merged our old `AnimBP_Player` block** (2.0.2 or the frozen 2.0.4 one):
it is harmless and you can remove it. It only acts on `Movement Input Vector` in
dialogue, and during dialogue our layer draws the body anyway.

### Detecting Immersive Dialogue

If you want to know whether we are installed, load this soft object path:

```
/Game/ImmersiveDialogueCompat/ID_AnimInterface_v1.ID_AnimInterface_v1
```

It is an empty curve asset shipped in the main pak from 2.0.4, and its name is
an **interface revision, not a mod version**: `v1` means our player character is
present, feeds `Movement Input Vector` during dialogue and owns the dialogue
camera. It stays `v1` across releases for as long as that holds.

Do **not** detect us through `/Game/__ModKitWwiseCookAnchor_ImmersiveDialogue_<number>__`.
The Zone Kit generates that asset on every cook with a new number, so it matches
exactly one build.

The UE4SS version (1.x) replaces no assets and needs none of this.

Everything here is MIT-licensed; merge what you need. Questions and patches:
GitHub issues, or the Nexus page (mod 2698).

## Versions

- **2.1.0** — no longer replaces the player animation file, so watch and
  animation mods (ZST and others) work alongside with no patch. Skif's arms are
  visible in dialogue and gestures play on them. Dialogue movement follows the
  keys you bound in Options > Controls (AZERTY and other layouts), including
  during the zoom-in.
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
- **1.0.x** — UE4SS C++ DLL. Still available under the `v1.0.8` tag and
  earlier releases; source in `ImmersiveDialogueCpp/`. Bug fixes only
  (1.0.8: no more walking off on your own after a conversation you did not
  move in).

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
