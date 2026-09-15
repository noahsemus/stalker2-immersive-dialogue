# Zone Kit tooling (no editor clicking required)

All scripts run with the kit's embedded Python:
`G:\Epic Games\STALKER2ZoneKit\Engine\Binaries\ThirdParty\Python3\Win64\python.exe`.
Replace `<SCRATCH>` with a writable output folder.

- `extract_from_pak.py` — pulls uncooked assets straight out of
  `FullEditor-WindowsModEditor.pak` by index offset (the pak is unencrypted;
  entry header is 53 bytes). Needs a `pak_index_subset.txt` produced by
  `UnrealPak.exe <pak> -List | grep <paths>`.
- `dump_names.py` — poor man's `strings` for a `.uasset` (key / action names).
- `zen_names.py` — FName table or imported package paths (`--imports`) of
  *cooked* packages, for reading other mods' paks. Extract first with
  `<kit>\Engine\Binaries\Win64\UnrealPak.exe <mod>.utoc -Extract <dir>`, then
  `zen_names.py <dir> [--imports] [--filter REGEX]`.
- `dump_imc.py`, `make_imc_override.py` — run *inside* the editor:
  ```
  Stalker2ModEditor-Win64-Shipping-Cmd.exe "<kit>\Stalker2\Stalker2.uproject" -run=pythonscript -script=<file> -unattended -nosplash -stdout -NoShaderCompile
  ```
  A headless run takes ~4.5 min to boot; the mod plugin content
  (`/ImmersiveDialogue/`) is mounted automatically.
- `ue_exec.py` — same scripts in seconds via the running editor once
  *Editor Preferences → Plugins → Python → Enable Remote Execution* is on.

Cook + pak (what the editor's *Package Mod* button runs):
```
RunUAT.bat GSCCookMod "-Project=<kit>\Stalker2\Stalker2.uproject" "-PluginPath=<kit>\Stalker2\Mods\ImmersiveDialogue\ImmersiveDialogue.uplugin" "-PackageClassifierOutputDir=<kit>\Stalker2\SavedMods\PackageClassifier\ImmersiveDialogue" -TargetPlatform=Win64 -nocompile -nocompileuat
```
`OverridePackages.txt` / `NewPackages.txt` in that classifier dir list the
packages to cook (the editor normally writes them).
Output: `Stalker2\SavedMods\Install\ImmersiveDialogue\Content\Paks\Windows\*OverrideContent.{pak,ucas,utoc}`.
