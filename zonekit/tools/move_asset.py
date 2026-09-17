r"""Rename/move an asset inside the mod plugin and fix up the Blueprints that reference it.

Use this instead of editing pins by hand: rename_asset updates referencers, and the script
saves them plus deletes the redirector left at the old path (a redirector inside the mod's
Content would otherwise get cooked).

Edit SRC / DST / LOG, then run with the editor CLOSED (~5 min):

  <kit>\Stalker2\Binaries\Win64\Stalker2ModEditor-Win64-Shipping-Cmd.exe "<kit>\Stalker2\Stalker2.uproject"
      -run=pythonscript -script=<repo>\zonekit\tools\move_asset.py -unattended -nosplash -stdout -NoShaderCompile
"""
import os
import unreal

SRC = "/ImmersiveDialogue/Runtime/AnimBP_PlayerDialogue"
DST = "/ImmersiveDialogue/_STALKER2/Animations/Player/AnimBP_PlayerDialogue"
LOG = r"C:\Users\noahs\AppData\Local\Temp\move_asset.log"
UPLUGIN = r"G:/Epic Games/STALKER2ZoneKit/Stalker2/Mods/ImmersiveDialogue/ImmersiveDialogue.uplugin"

lines = []
def log(s):
    lines.append(str(s)); unreal.log("[ImmDlg] " + str(s))

EAL = unreal.EditorAssetLibrary
try:
    # A headless run mounts neither the plugin nor its assets into the registry.
    if not EAL.does_asset_exist("/ImmersiveDialogue/ImmersiveDialogue"):
        url = "file:" + os.path.abspath(UPLUGIN).replace("\\", "/")
        try:
            unreal.GameFeaturesSubsystem.load_and_activate_game_feature_plugin(url, unreal.GameFeaturePluginLoadComplete())
        except Exception as e:
            log(f"load_and_activate failed: {e}")
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    ar.scan_paths_synchronous(["/ImmersiveDialogue"], True)
    log(f"before: {[str(a.package_name) for a in ar.get_assets_by_path('/ImmersiveDialogue', recursive=True)]}")

    refs = ar.get_referencers(SRC, unreal.AssetRegistryDependencyOptions())
    log(f"referencers of src: {[str(r) for r in (refs or [])]}")

    ok = EAL.rename_asset(SRC, DST)
    log(f"rename -> {ok}")
    if not ok:
        raise RuntimeError("rename_asset failed")

    if EAL.does_asset_exist(SRC):          # redirector left behind
        log(f"deleting redirector at src: {EAL.delete_asset(SRC)}")
    log(f"save dirty assets: {EAL.save_directory('/ImmersiveDialogue', only_if_is_dirty=True, recursive=True)}")

    ar.scan_paths_synchronous(["/ImmersiveDialogue"], True)
    log(f"after: {[str(a.package_name) for a in ar.get_assets_by_path('/ImmersiveDialogue', recursive=True)]}")
except Exception:
    import traceback
    log("EXCEPTION: " + traceback.format_exc())
open(LOG, "w", encoding="utf-8").write("\n".join(lines))
