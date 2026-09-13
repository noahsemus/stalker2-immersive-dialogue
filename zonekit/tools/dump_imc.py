import unreal, json
OUT = r"<SCRATCH>/imc_dump.json"
out = []
for path in ["/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Exploration",
             "/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Dialog",
             "/Game/_Stalker_2/data/input/InputMappingContexts/IMC_DialogOnTheGo",
             "/Game/_Stalker_2/data/input/InputMappingContexts/IMC_Interactivity"]:
    imc = unreal.load_asset(path)
    if not imc:
        out.append({"imc": path, "error": "load failed"}); continue
    for m in imc.get_editor_property("mappings"):
        act = m.get_editor_property("action")
        key = m.get_editor_property("key")
        def desc(o):
            if o is None: return None
            d = {"class": o.get_class().get_name()}
            for prop in ("order", "value_type", "deadzone", "lower_threshold", "upper_threshold", "type", "scalar", "hold_time_threshold"):
                try: d[prop] = str(o.get_editor_property(prop))
                except Exception: pass
            return d
        out.append({"imc": path.rsplit("/",1)[1],
                    "action": act.get_name() if act else None,
                    "action_value_type": str(act.get_editor_property("value_type")) if act else None,
                    "key": str(key.get_editor_property("key_name")),
                    "modifiers": [desc(x) for x in m.get_editor_property("modifiers")],
                    "triggers": [desc(x) for x in m.get_editor_property("triggers")]})
open(OUT, "w", encoding="utf-8").write(json.dumps(out, indent=1))
unreal.log("IMC dump written: %d rows" % len(out))
