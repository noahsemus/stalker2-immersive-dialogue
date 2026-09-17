r"""Duplicate one asset inside the mod plugin, headlessly (Blueprint graphs survive; a Move would
leave a redirector at the old path, which would still act as an override).

Edit SRC / DST / LOG below, then run (editor closed is fine, ~4.5 min to boot):

  <kit>\Stalker2\Binaries\Win64\Stalker2ModEditor-Win64-Shipping-Cmd.exe "<kit>\Stalker2\Stalker2.uproject"
      -run=pythonscript -script=<repo>\zonekit\tools\duplicate_asset.py -unattended -nosplash -stdout -NoShaderCompile
"""
import unreal

SRC = "/ImmersiveDialogue/_STALKER2/Animations/Player/AnimBP_Player"
DST = "/ImmersiveDialogue/Runtime/AnimBP_PlayerDialogue"
LOG = r"C:\Users\noahs\AppData\Local\Temp\duplicate_asset.log"

lines = []
def log(s):
    lines.append(str(s)); unreal.log("[ImmDlg] " + str(s))

EAL = unreal.EditorAssetLibrary
UPLUGIN = r"G:/Epic Games/STALKER2ZoneKit/Stalker2/Mods/ImmersiveDialogue/ImmersiveDialogue.uplugin"

def mount_mod_content():
    """A headless run does not mount the mod plugin's content; activate it like make_imc_override.py does."""
    if EAL.does_asset_exist("/ImmersiveDialogue/ImmersiveDialogue"):
        return True
    import os
    url = "file:" + os.path.abspath(UPLUGIN).replace("\\", "/")
    try:
        unreal.GameFeaturesSubsystem.load_and_activate_game_feature_plugin(url, unreal.GameFeaturePluginLoadComplete())
    except Exception as e:
        log(f"load_and_activate failed: {e}")
    return EAL.does_asset_exist("/ImmersiveDialogue/ImmersiveDialogue")

try:
    log(f"mod content mounted: {mount_mod_content()}")
    # The registry does not scan plugin content on its own in a headless run.
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    ar.scan_paths_synchronous(["/ImmersiveDialogue"], True)
    found = [str(a.package_name) for a in ar.get_assets_by_path("/ImmersiveDialogue", recursive=True)]
    log(f"assets under /ImmersiveDialogue ({len(found)}): {found}")
    log(f"src exists: {EAL.does_asset_exist(SRC)}")
    if EAL.does_asset_exist(DST):
        log("dst exists already, deleting it first")
        EAL.delete_asset(DST)
    dup = EAL.duplicate_asset(SRC, DST)
    log(f"duplicate -> {dup}")
    if dup is None:
        raise RuntimeError("duplicate_asset returned None")
    ok = EAL.save_asset(DST, only_if_is_dirty=False)
    log(f"saved: {ok}")
    log(f"verify: {EAL.does_asset_exist(DST)}  class={unreal.load_asset(DST).get_class().get_name()}")
except Exception:
    import traceback
    log("EXCEPTION: " + traceback.format_exc())
open(LOG, "w", encoding="utf-8").write("\n".join(lines))
