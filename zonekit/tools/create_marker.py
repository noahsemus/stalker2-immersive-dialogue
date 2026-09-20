r"""Create the stable compatibility marker asset other mods can detect (editor CLOSED, ~5 min).

  <kit>\Stalker2\Binaries\Win64\Stalker2ModEditor-Win64-Shipping-Cmd.exe "<kit>\Stalker2\Stalker2.uproject"
      -run=pythonscript -script=<repo>\zonekit\tools\create_marker.py -unattended -nosplash -stdout -NoShaderCompile

The marker is an empty CurveFloat. In the mod folder it lives at
/ImmersiveDialogue/ImmersiveDialogueCompat/ID_AnimInterface_v1; listed in OverridePackages.txt it cooks
to /Game/ImmersiveDialogueCompat/ID_AnimInterface_v1, which is the path other mods load. Never rename it.
"""
import os
import unreal

FOLDER = "/ImmersiveDialogue/ImmersiveDialogueCompat"
NAME   = "ID_AnimInterface_v1"
LOG    = r"C:\Users\noahs\AppData\Local\Temp\create_marker.log"
UPLUGIN = r"G:/Epic Games/STALKER2ZoneKit/Stalker2/Mods/ImmersiveDialogue/ImmersiveDialogue.uplugin"

lines = []
def log(s):
    lines.append(str(s)); unreal.log("[ImmDlg] " + str(s))

EAL = unreal.EditorAssetLibrary
try:
    if not EAL.does_asset_exist("/ImmersiveDialogue/ImmersiveDialogue"):
        url = "file:" + os.path.abspath(UPLUGIN).replace(chr(92), "/")
        try:
            unreal.GameFeaturesSubsystem.load_and_activate_game_feature_plugin(url, unreal.GameFeaturePluginLoadComplete())
        except Exception as e:
            log(f"load_and_activate failed: {e}")
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(["/ImmersiveDialogue"], True)
    path = FOLDER + "/" + NAME
    if EAL.does_asset_exist(path):
        log("marker exists already")
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        asset = None
        for make in (lambda: unreal.CurveFloatFactory(), lambda: unreal.CurveFactory()):
            try:
                f = make()
                try: f.set_editor_property("curve_class", unreal.CurveFloat)
                except Exception: pass
                asset = tools.create_asset(NAME, FOLDER, unreal.CurveFloat, f)
                if asset: break
            except Exception as e:
                log(f"factory attempt failed: {e}")
        log(f"created: {asset}")
        if not asset:
            raise RuntimeError("could not create marker")
        log(f"saved: {EAL.save_loaded_asset(asset, only_if_is_dirty=False)}")
    log(f"exists: {EAL.does_asset_exist(path)}")
except Exception:
    import traceback
    log("EXCEPTION: " + traceback.format_exc())
open(LOG, "w", encoding="utf-8").write("\n".join(lines))
