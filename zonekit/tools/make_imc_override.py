"""Regenerates the mod's IMC_Dialog override (BUILD.md 5.2). Runs inside the editor:

  Stalker2ModEditor-Win64-Shipping-Cmd.exe "<kit>\Stalker2\Stalker2.uproject" -run=pythonscript -script=<this file> -unattended -nosplash -stdout -NoShaderCompile

or in seconds through ue_exec.py against a running editor. Edit LOG first.

What it does:
  1. duplicates the vanilla IMC_Dialog into the mod plugin at the mirrored path;
  2. drops the four IA_UI_Dialog_SelectAnswer rows on W / S / left stick up / down;
  3. duplicates IMC_Exploration to a temporary asset and MOVES (rename outer) the
     modifier, trigger and player-mappable-settings objects of every
     IA_LocomotionForward / IA_LookUp row into the override, then adds a copy of each
     row. Moving the real objects keeps the game's dead zones, response curves and the
     custom ApplySensitivity modifier intact (new_object + set_editor_property fails
     with "cannot be edited on templates"). Moving the mappable settings keeps each
     move row's PlayerMappableOption name (MoveForward / ...), which is how the
     game's own Options > Controls rebind (CustomizeControls.cfg) reaches the row:
     without it the dialogue rows were frozen on W / A / S / D and an AZERTY player's
     ZQSD did nothing in dialogue (Nexus, TheChillPakBoi, 2026-09-21);
  4. saves the override. The temp asset is never saved; do NOT delete_asset it
     (crashes the commandlet).

IMMDLG_LAYOUT=azerty in the environment builds the fallback variant with Z / Q in
place of W / A on the copied move rows (only needed if step 3's mappable settings turn
out not to be enough in game; see BUILD.md 5.2).
"""
import unreal, os, time
LOG = r"<SCRATCH>/make_imc_override.log"
LAYOUT = os.environ.get("IMMDLG_LAYOUT", "qwerty").strip().lower()
KEY_SWAP = {"azerty": {"W": "Z", "A": "Q"}}.get(LAYOUT, {})
lines = []
def log(s):
    lines.append(str(s)); unreal.log("[ImmDlg] " + str(s))
SRC_DIALOG = "/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Dialog"
SRC_EXPLO  = "/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Exploration"
DST_DIR    = "/ImmersiveDialogue/_Stalker_2/data/input/InputMappingContexts"
DST        = DST_DIR + "/IMC_Dialog"
TMP        = DST_DIR + "/IMC_ExplorationTmp_%d" % int(time.time())   # unique per run, never saved
EAL = unreal.EditorAssetLibrary

def dup(src, dst):
    """EditorAssetLibrary.duplicate_asset returns None inside a running editor for /Game
    sources; AssetTools.duplicate_asset works in both the commandlet and the live editor."""
    r = EAL.duplicate_asset(src, dst)
    if r is None:
        folder, name = dst.rsplit("/", 1)
        r = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(name, folder, unreal.load_asset(src))
    return r

def prop(o, name, default=None):
    """get_editor_property that returns `default` when the property doesn't exist."""
    try:
        return o.get_editor_property(name)
    except Exception:
        return default

def mappable_desc(m):
    beh = prop(m, "setting_behavior")
    s = prop(m, "player_mappable_key_settings")
    nm = prop(s, "name") if s else None
    return "%s/%s" % (str(beh).rsplit(".", 1)[-1] if beh is not None else "-", nm if nm else "-")

def describe(m):
    a = prop(m, "action"); k = prop(m, "key")
    return "%-28s %-26s mappable=%-32s mods=%s trig=%s" % (
        a.get_name() if a else None, str(k.get_editor_property("key_name")), mappable_desc(m),
        [x.get_class().get_name() for x in prop(m, "modifiers", []) if x],
        [x.get_class().get_name() for x in prop(m, "triggers", []) if x])

try:
    log("layout=%s key swap=%s" % (LAYOUT, KEY_SWAP))
    # 1. Is the mod plugin content mounted?
    mounted = EAL.does_asset_exist("/ImmersiveDialogue/ImmersiveDialogue")
    log(f"mod content mounted: {mounted}")
    if not mounted:
        url = "file:" + os.path.abspath(r"G:/Epic Games/STALKER2ZoneKit/Stalker2/Mods/ImmersiveDialogue/ImmersiveDialogue.uplugin").replace("\\", "/")
        try:
            unreal.GameFeaturesSubsystem.load_and_activate_game_feature_plugin(url, unreal.GameFeaturePluginLoadComplete())
        except Exception as e:
            log(f"load_and_activate failed: {e}")
        mounted = EAL.does_asset_exist("/ImmersiveDialogue/ImmersiveDialogue")
        log(f"mod content mounted after activate: {mounted}")
    if not mounted:
        raise RuntimeError("mod content not mounted; cannot create override")

    # 2. Duplicate IMC_Dialog into the mod at the mirrored path.
    if EAL.does_asset_exist(DST):
        log("override exists already, deleting to rebuild")
        EAL.delete_asset(DST)
    dup = dup(SRC_DIALOG, DST)
    log(f"duplicate -> {dup}")
    imc = unreal.load_asset(DST)

    # 3. Remove the mappings that fight free movement.
    REMOVE = {("IA_UI_Dialog_SelectAnswer", "W"), ("IA_UI_Dialog_SelectAnswer", "S"),
              ("IA_UI_Dialog_SelectAnswer", "Gamepad_LeftStick_Up"), ("IA_UI_Dialog_SelectAnswer", "Gamepad_LeftStick_Down")}
    kept = []
    removed = 0
    for m in imc.get_editor_property("mappings"):
        a = m.get_editor_property("action"); k = m.get_editor_property("key")
        sig = (a.get_name() if a else None, str(k.get_editor_property("key_name")))
        if sig in REMOVE: removed += 1; continue
        kept.append(m)
    log(f"removed {removed} mappings, kept {len(kept)}")

    # 4. Move the move + look rows' objects out of a throwaway copy of IMC_Exploration.
    COPY_ACTIONS = {"IA_LocomotionForward", "IA_LookUp"}
    tmp_dup = dup(SRC_EXPLO, TMP)
    log(f"temp exploration copy -> {tmp_dup}")
    tmp = unreal.load_asset(TMP)
    def take(o):
        """Re-parent a subobject of the temp copy into the override; None stays None."""
        if o is None: return None
        ok = o.rename(outer=imc)
        if not ok: log(f"   rename(outer=override) FAILED for {o.get_class().get_name()}")
        return o
    added = 0; mappable = 0
    for m in tmp.get_editor_property("mappings"):
        a = m.get_editor_property("action")
        if not a or a.get_name() not in COPY_ACTIONS: continue
        k = m.get_editor_property("key")
        kname = str(k.get_editor_property("key_name"))
        if kname in ("None", "RotationRate"): continue
        if kname in KEY_SWAP:
            k = unreal.Key(); k.set_editor_property("key_name", KEY_SWAP[kname])
            log(f"  layout swap {kname} -> {KEY_SWAP[kname]}")
        nm = unreal.EnhancedActionKeyMapping()
        nm.set_editor_property("action", a)
        nm.set_editor_property("key", k)
        nm.set_editor_property("modifiers", [take(x) for x in m.get_editor_property("modifiers")])
        nm.set_editor_property("triggers",  [take(x) for x in m.get_editor_property("triggers")])
        # Player-mappable settings (UE 5.3+ object + behaviour). The game keys its
        # CustomizeControls.cfg rows on the settings' Name (PlayerMappableOption), so the
        # copied row must carry the same object as the exploration row.
        beh = prop(m, "setting_behavior")
        if beh is not None:
            try: nm.set_editor_property("setting_behavior", beh)
            except Exception as e: log(f"   set setting_behavior failed: {e}")
        s = prop(m, "player_mappable_key_settings")
        if s is not None:
            try:
                nm.set_editor_property("player_mappable_key_settings", take(s)); mappable += 1
            except Exception as e: log(f"   set player_mappable_key_settings failed: {e}")
        # Legacy (pre-5.3) struct, editor-only data; best effort so the two never disagree.
        legacy = prop(m, "player_mappable_options")
        if legacy is not None:
            try: nm.set_editor_property("player_mappable_options", legacy)
            except Exception as e: log(f"   set player_mappable_options failed: {e}")
        log(f"  + {describe(nm)}")
        kept.append(nm); added += 1
    imc.set_editor_property("mappings", kept)
    log(f"added {added} mappings ({mappable} with mappable settings); total now {len(kept)}")
    if mappable == 0:
        log("WARNING: no copied row carried player-mappable settings; the game's Move rebind "
            "will not reach the dialogue rows. Check dump_imc.py on IMC_Exploration.")

    # 5. Save the override only. The temp copy is left unsaved (never on disk).
    ok = EAL.save_loaded_asset(imc, only_if_is_dirty=False)
    log(f"saved: {ok}")
    # 6. Verify by re-reading.
    imc2 = unreal.load_asset(DST)
    for m in imc2.get_editor_property("mappings"):
        log("  VERIFY " + describe(m))
except Exception as e:
    import traceback
    log("EXCEPTION: " + traceback.format_exc())
open(LOG, "w", encoding="utf-8").write("\n".join(lines))
