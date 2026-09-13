import re, os, sys
PAK = r"G:/Epic Games/STALKER2ZoneKit/Stalker2/Content/Paks/FullEditor-WindowsModEditor.pak"
idx = open("pak_index_subset.txt", encoding="utf-8", errors="replace").read()
want = ["InputMappingContexts/IMC_Dialog.uasset","InputMappingContexts/IMC_DialogOnTheGo.uasset","InputMappingContexts/IMC_Exploration.uasset","InputMappingContexts/IMC_NoInput.uasset","Player/BP_Stalker2Character.uasset","Dialogue/W_DialogueView.uasset","Delayable/IA_Walk.uasset","Delayable/IA_LookUp.uasset","Delayable/IA_LocomotionForward.uasset"]
os.makedirs("extracted", exist_ok=True)
MAGIC = bytes.fromhex("C1832A9E")
with open(PAK, "rb") as f:
    for w in want:
        m = re.search(r'"([^"]*%s)" offset: (\d+), size: (\d+) bytes, sha1: [0-9A-F]+, compression: (\w+)' % re.escape(w), idx)
        if not m: print("MISSING", w); continue
        path, off, size, comp = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
        f.seek(off); head = f.read(128)
        k = head.find(MAGIC)
        if comp != "None" or k < 0:
            print(f"SKIP {w}: comp={comp} magic_at={k}"); continue
        f.seek(off + k); data = f.read(size)
        out = os.path.join("extracted", os.path.basename(path))
        open(out, "wb").write(data)
        print(f"OK {out} ({size} bytes, header {k})")
