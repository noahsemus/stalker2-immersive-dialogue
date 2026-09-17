r"""Set a default value on a Blueprint's class defaults headlessly (editor CLOSED, ~5 min).

Used for values that are awkward to enter in the editor UI. A SoftClassProperty must be given a class
that exists in the editor; the mod-folder path (/<Mod>/...) is what gets saved, and for override
packages the game resolves that name at runtime (the pawn's IMC_Dialog reference works the same way).
Edit BP / PROP / VALUE / LOG, then:

  <kit>\Stalker2\Binaries\Win64\Stalker2ModEditor-Win64-Shipping-Cmd.exe "<kit>\Stalker2\Stalker2.uproject"
      -run=pythonscript -script=<repo>\zonekit\tools\set_bp_default.py -unattended -nosplash -stdout -NoShaderCompile
"""
import os
import unreal

BP    = "/ImmersiveDialogue/GameLite/Blueprints/Characters/Player/BP_Stalker2Character"
PROP  = "DlgAnimClassSoft"
VALUE = "/ImmersiveDialogue/_STALKER2/Animations/Player/AnimBP_PlayerDialogue.AnimBP_PlayerDialogue_C"
LOG   = r"C:\Users\noahs\AppData\Local\Temp\set_bp_default.log"
UPLUGIN = r"G:/Epic Games/STALKER2ZoneKit/Stalker2/Mods/ImmersiveDialogue/ImmersiveDialogue.uplugin"

lines = []
def log(s):
    lines.append(str(s)); unreal.log("[ImmDlg] " + str(s))

EAL = unreal.EditorAssetLibrary
try:
    if not EAL.does_asset_exist("/ImmersiveDialogue/ImmersiveDialogue"):
        url = "file:" + os.path.abspath(UPLUGIN).replace("\\", "/")
        try:
            unreal.GameFeaturesSubsystem.load_and_activate_game_feature_plugin(url, unreal.GameFeaturePluginLoadComplete())
        except Exception as e:
            log(f"load_and_activate failed: {e}")
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(["/ImmersiveDialogue"], True)

    cls = unreal.load_class(None, BP + "." + BP.rsplit("/", 1)[1] + "_C")
    log(f"class: {cls}")
    cdo = unreal.get_default_object(cls)
    log(f"cdo: {cdo}")
    log(f"before: {cdo.get_editor_property(PROP)}")
    done = False
    # A SoftClassProperty only accepts a real class object from Python; the saved value is its soft path.
    for make in (lambda: unreal.load_class(None, VALUE), lambda: unreal.SoftClassPath(VALUE)):
        try:
            cdo.set_editor_property(PROP, make()); done = True; break
        except Exception as e:
            log(f"set attempt failed: {e}")
    if not done:
        raise RuntimeError("could not set property")
    log(f"after: {cdo.get_editor_property(PROP)}")
    log(f"saved: {EAL.save_asset(BP, only_if_is_dirty=False)}")
    cdo2 = unreal.get_default_object(unreal.load_class(None, BP + "." + BP.rsplit("/", 1)[1] + "_C"))
    log(f"reload check: {cdo2.get_editor_property(PROP)}")
except Exception:
    import traceback
    log("EXCEPTION: " + traceback.format_exc())
open(LOG, "w", encoding="utf-8").write("\n".join(lines))
