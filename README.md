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

1. Download `ImmersiveDialogue-v2.0.0.zip` from Nexus or this repo's Releases
   page and drop it into Vortex.
2. Deploy. Done.

### Manual

1. Get `ImmersiveDialogue-v2.0.0.zip` from the latest release.
2. Copy the three files
   ```
   zzz_ImmersiveDialogue_20_P.pak
   zzz_ImmersiveDialogue_20_P.ucas
   zzz_ImmersiveDialogue_20_P.utoc
   ```
   into `<GAME>\Stalker2\Content\Paks\~mods\` (create `~mods` if needed).
   Keep the three names identical apart from the extension.

To uninstall, delete the three files.

## Known limitations

- **2.0.0: quick slots (Q/E) or pad face buttons can stop responding after a
  dialogue** (typically after a trade). Reloading a save clears it. Fixed in
  2.0.1 (in progress).
- **Strafe start in dialogue.** For roughly half a second after you start
  strafing from a standstill inside a dialogue, the body plays the walk-start
  before settling into the strafe. Cosmetic.
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
- Every game patch requires this mod to be rebuilt against the new Zone Kit.
  If a patch breaks it, check the Nexus page for an update.
- Save games are unaffected; the pawn class keeps its vanilla path.

## Versions

- **2.0.0** — rewrite as a Zone Kit pak. No UE4SS. Everything the 1.x DLL
  did, plus proper walk/strafe animation in dialogue and controller support
  through the game's own input system.
- **1.0.x** — UE4SS C++ DLL. Still available under the `v1.0.4` tag and
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
