import unreal, json, os
LOG = r"<SCRATCH>/make_imc_override.log"
lines = []
def log(s):
    lines.append(str(s)); unreal.log("[ImmDlg] " + str(s))
SRC_DIALOG = "/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Dialog"
SRC_EXPLO  = "/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Exploration"
DST_DIR    = "/ImmersiveDialogue/_Stalker_2/data/input/InputMappingContexts"
DST        = DST_DIR + "/IMC_Dialog"
EAL = unreal.EditorAssetLibrary
try:
    # 1. Is the mod plugin content mounted?
    mounted = EAL.does_asset_exist("/ImmersiveDialogue/ImmersiveDialogue")
    log(f"mod content mounted: {mounted}")
    if not mounted:
        gfs = unreal.GameFeaturesSubsystem.get_game_feature_subsystem() if hasattr(unreal, "GameFeaturesSubsystem") else None
        log(f"GameFeaturesSubsystem: {gfs}")
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
    dup = EAL.duplicate_asset(SRC_DIALOG, DST)
    log(f"duplicate -> {dup}")
    imc = unreal.load_asset(DST)
    expl = unreal.load_asset(SRC_EXPLO)

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

    # 4. Copy the move + look mappings from IMC_Exploration (with cloned modifiers/triggers).
    COPY_ACTIONS = {"IA_LocomotionForward", "IA_LookUp"}
    PROPS = ["lower_threshold", "upper_threshold", "type", "order", "scalar", "curve", "response_curve",
             "exponent", "hold_time_threshold", "actuation_threshold", "is_one_shot", "sensitivity",
             "acceleration_curve", "deadzone", "max_value", "min_value", "curve_exponent", "invert", "axis", "input_type"]
    def clone(o):
        if o is None: return None
        n = unreal.new_object(o.get_class(), outer=imc)
        copied = []
        for p in PROPS:
            try:
                v = o.get_editor_property(p)
            except Exception:
                continue
            try:
                n.set_editor_property(p, v); copied.append(p)
            except Exception as e:
                log(f"   set {p} on {o.get_class().get_name()} failed: {e}")
        log(f"   cloned {o.get_class().get_name()} props={copied}")
        return n
    added = 0
    for m in expl.get_editor_property("mappings"):
        a = m.get_editor_property("action")
        if not a or a.get_name() not in COPY_ACTIONS: continue
        k = m.get_editor_property("key")
        if str(k.get_editor_property("key_name")) in ("None", "RotationRate"): continue
        nm = unreal.EnhancedActionKeyMapping()
        nm.set_editor_property("action", a)
        nm.set_editor_property("key", k)
        nm.set_editor_property("modifiers", [clone(x) for x in m.get_editor_property("modifiers")])
        nm.set_editor_property("triggers",  [clone(x) for x in m.get_editor_property("triggers")])
        log(f"  + {a.get_name()} <- {k.get_editor_property('key_name')}")
        kept.append(nm); added += 1
    imc.set_editor_property("mappings", kept)
    log(f"added {added} mappings; total now {len(kept)}")

    # 5. Save.
    ok = EAL.save_loaded_asset(imc, only_if_is_dirty=False)
    log(f"saved: {ok}")
    # 6. Verify by re-reading.
    imc2 = unreal.load_asset(DST)
    for m in imc2.get_editor_property("mappings"):
        a = m.get_editor_property("action"); k = m.get_editor_property("key")
        log(f"  VERIFY {a.get_name() if a else None:28s} {str(k.get_editor_property('key_name')):26s} mods={[x.get_class().get_name() for x in m.get_editor_property('modifiers') if x]} trig={[x.get_class().get_name() for x in m.get_editor_property('triggers') if x]}")
except Exception as e:
    import traceback
    log("EXCEPTION: " + traceback.format_exc())
open(LOG, "w", encoding="utf-8").write("\n".join(lines))
