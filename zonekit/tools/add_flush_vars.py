"""Adds the two Booleans the body-layer notify flush (gen_subsystem_notify_flush.py) needs to BP_ImmDlgSubsystem.

Run headless with the editor closed (harness run_headless.ps1 -Mod ImmersiveDialogue), or in the open editor via
ue_exec.py. Skips variables that already exist. Writes a result line to <SCRATCH>/add_flush_vars.txt.
"""
import os
import unreal

BP = "/ImmersiveDialogue/Runtime/BP_ImmDlgSubsystem"
NAMES = ("BodyLayerWasOn", "BodyLayerFlush")

bp = unreal.load_asset(BP)
if bp is None:
    raise RuntimeError("load failed: " + BP)
lib = unreal.BlueprintEditorLibrary
t = lib.get_basic_type_by_name("bool")
gen = lib.generated_class(bp)
have = set()
if gen:
    cdo = unreal.get_default_object(gen)
    for n in NAMES:
        try:
            cdo.get_editor_property(n)
            have.add(n)
        except Exception:
            pass
added = []
for n in NAMES:
    if n in have:
        continue
    if lib.add_member_variable(bp, n, t):
        added.append(n)
lib.compile_blueprint(bp)
saved = unreal.EditorAssetLibrary.save_asset(BP, only_if_is_dirty=False)
os.makedirs(SCRATCH, exist_ok=True)
with open(os.path.join(SCRATCH, "add_flush_vars.txt"), "w") as f:
    f.write(f"had={sorted(have)} added={added} saved={saved}\n")
