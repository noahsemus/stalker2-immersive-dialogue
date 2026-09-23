"""Give the dialogue override's move rows the same player-mappable names as the exploration
rows (MoveForward, MoveBack, MoveLeft, MoveRight, and the *Alt secondary slots), so a rebind
in Options > Controls (stored by name in CustomizeControls.cfg) also moves the dialogue rows.

Runs inside the live editor (zonekit/tools/ue_exec.py) on the existing override, in place.
make_imc_override.py does the same while building the asset from scratch, but its
duplicate_asset step only works in the commandlet (returns None in a running editor).
Idempotent: rows that already carry settings are left alone; Alt rows are added once.
"""
import unreal
DST = "/ImmersiveDialogue/_Stalker_2/data/input/InputMappingContexts/IMC_Dialog"
SRC_EXPLO = "/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Exploration"
AR = unreal.AssetRegistryHelpers.get_asset_registry()
AR.scan_paths_synchronous([DST.rsplit("/", 1)[0]], True)
imc = unreal.load_asset(DST); ex = unreal.load_asset(SRC_EXPLO)
def keyname(m): return str(m.get_editor_property("key").get_editor_property("key_name"))
def act(m):
    a = m.get_editor_property("action"); return a.get_name() if a else None
SETTINGS_PROPS = ("name", "display_name", "display_category", "metadata")
def clone_settings(src):
    t = unreal.new_object(unreal.PlayerMappableKeySettings, imc)
    for p in SETTINGS_PROPS:
        try: t.set_editor_property(p, src.get_editor_property(p))
        except Exception as e: print(f"   copy {p}: {e}")
    return t
# exploration move rows with mappable settings, by key (the four primaries) and the Alt slots
explo = [m for m in ex.get_editor_property("mappings") if act(m) == "IA_LocomotionForward" and m.get_editor_property("player_mappable_key_settings")]
by_key = {keyname(m): m for m in explo if keyname(m) != "None"}
alts = [m for m in explo if keyname(m) == "None"]
print("exploration mappable move rows:", [(keyname(m), m.get_editor_property("player_mappable_key_settings").get_editor_property("name")) for m in explo])
rows = list(imc.get_editor_property("mappings"))
have = {str(m.get_editor_property("player_mappable_key_settings").get_editor_property("name")) for m in rows if m.get_editor_property("player_mappable_key_settings")}
changed = 0
for i, m in enumerate(rows):
    if act(m) != "IA_LocomotionForward" or m.get_editor_property("player_mappable_key_settings"): continue
    src = by_key.get(keyname(m))
    if not src: continue
    m.set_editor_property("setting_behavior", src.get_editor_property("setting_behavior"))
    m.set_editor_property("player_mappable_key_settings", clone_settings(src.get_editor_property("player_mappable_key_settings")))
    rows[i] = m; changed += 1
    have.add(str(src.get_editor_property("player_mappable_key_settings").get_editor_property("name")))
# the secondary (Alt) slots: copy the dialogue row of the same direction, key None, Alt settings
dir_row = {}
for m in rows:
    s = m.get_editor_property("player_mappable_key_settings")
    if act(m) == "IA_LocomotionForward" and s: dir_row[str(s.get_editor_property("name"))] = m
for a in alts:
    nm = str(a.get_editor_property("player_mappable_key_settings").get_editor_property("name"))
    if nm in have: continue
    base = dir_row.get(nm[:-3])  # MoveBackAlt -> MoveBack
    if not base: print("   no base row for", nm); continue
    n = unreal.EnhancedActionKeyMapping()
    n.set_editor_property("action", base.get_editor_property("action"))
    n.set_editor_property("key", a.get_editor_property("key"))
    n.set_editor_property("modifiers", list(base.get_editor_property("modifiers")))
    n.set_editor_property("triggers", list(base.get_editor_property("triggers")))
    n.set_editor_property("setting_behavior", a.get_editor_property("setting_behavior"))
    n.set_editor_property("player_mappable_key_settings", clone_settings(a.get_editor_property("player_mappable_key_settings")))
    rows.append(n); changed += 1; have.add(nm)
imc.set_editor_property("mappings", rows)
print("changed rows:", changed)
print("saved:", unreal.EditorAssetLibrary.save_loaded_asset(imc, only_if_is_dirty=False))
for m in unreal.load_asset(DST).get_editor_property("mappings"):
    if act(m) == "IA_LocomotionForward":
        s = m.get_editor_property("player_mappable_key_settings")
        print("  VERIFY", keyname(m), m.get_editor_property("setting_behavior"), s.get_editor_property("name") if s else None,
              [x.get_class().get_name() for x in m.get_editor_property("modifiers") if x])
