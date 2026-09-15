r"""Names and imports of cooked (Zen / IoStore) packages, for checking other mods' paks.

Extract a container first with the kit's UnrealPak:
    <kit>\Engine\Binaries\Win64\UnrealPak.exe <mod>.utoc -Extract <outdir>
then point this at the .uheader files (or a folder of them):
    python zen_names.py <outdir> [--imports] [--filter REGEX]

Default output is each package's FName table (keys, function and variable names,
node names). --imports prints the imported package paths instead, which is how to
see which game assets a Blueprint or mapping context actually references.
Run with the kit's Python (Engine\Binaries\ThirdParty\Python3\Win64\python.exe).
"""
import os, re, struct, sys

def name_batch(d, o):
    num = struct.unpack_from("<I", d, o)[0]; o += 4
    if num == 0:
        return []
    o += 4 + 8 + 8 * num                      # string bytes, hash version, hashes
    hdrs = [struct.unpack_from(">H", d, o + 2 * i)[0] for i in range(num)]; o += 2 * num
    out = []
    for h in hdrs:
        ln = h & 0x7FFF
        if h & 0x8000:                        # UTF-16, aligned
            if o % 2: o += 1
            out.append(d[o:o + 2 * ln].decode("utf-16-le")); o += 2 * ln
        else:
            out.append(d[o:o + ln].decode("latin1")); o += ln
    return out

def names(d):
    has_ver = struct.unpack_from("<I", d, 0)[0]
    o = 52                                    # FZenPackageSummary (UE 5.5)
    if has_ver:
        o += 4 + 8 + 4
        n = struct.unpack_from("<i", d, o)[0]; o += 4 + 20 * n
    return name_batch(d, o)

def imports(d):
    return name_batch(d, struct.unpack_from("<i", d, 48)[0])   # ImportedPackageNamesOffset

args = sys.argv[1:]
want_imports = "--imports" in args
flt = None
if "--filter" in args:
    i = args.index("--filter"); flt = re.compile(args[i + 1]); del args[i:i + 2]
paths = []
for a in (x for x in args if not x.startswith("--")):
    if os.path.isdir(a):
        paths += sorted(os.path.join(dp, f) for dp, _, fs in os.walk(a) for f in fs if f.endswith(".uheader"))
    else:
        paths.append(a)
for p in paths:
    d = open(p, "rb").read()
    try:
        items = imports(d) if want_imports else names(d)
    except Exception as e:
        print(f"=== {p}: unreadable ({e})"); continue
    if flt:
        items = [x for x in items if flt.search(x)]
    print(f"=== {p}")
    for x in items:
        print("   ", x)
