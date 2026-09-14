"""String dump of .uasset files: which actions / keys / modifiers does it reference?

For an uncooked editor package the FName table is parsed properly, so 1-2 character
key names (`Q`, `E`, `W`, `S`, ...) show up too. The plain byte scan (>= 3 printable
chars) is the fallback for cooked assets or anything that doesn't parse.
"""
import re, struct, sys

KEY_RE = re.compile(r'^(Gamepad_|Mouse|LeftMouseButton|RightMouseButton|MiddleMouseButton|[A-Z]$|Up$|Down$|Left$|Right$|Space|Escape|Enter|Tab|Left(Shift|Control|Alt)|Right(Shift|Control|Alt)|F\d+$|Zero|One|Two|Three|Four|Five|Six|Seven|Eight|Nine|Thumb|Caps|BackSpace|Delete|Insert|Home|End|PageUp|PageDown|NumPad)')
MOD_RE = re.compile(r'^(InputModifier|InputTrigger|Swizzle|Negate|DeadZone|Scalar|SmoothDelta|Pressed|Released|Hold|Tap)')

def name_table(data):
    """FName table of an uncooked UE4/UE5 package, or None if the header doesn't parse."""
    try:
        tag, legacy = struct.unpack_from("<Ii", data, 0)
        if tag != 0x9E2A83C1 or legacy > -5:
            return None
        o = 8
        if legacy != -4:
            o += 4                      # LegacyUE3Version
        o += 12 if legacy <= -8 else 8  # FileVersionUE4 [, UE5], Licensee
        n = struct.unpack_from("<i", data, o)[0]; o += 4 + 20 * n   # custom versions
        o += 4                          # TotalHeaderSize
        l = struct.unpack_from("<i", data, o)[0]; o += 4 + (l if l >= 0 else -2 * l)  # package name
        o += 4                          # PackageFlags
        count, off = struct.unpack_from("<ii", data, o)
        names = []
        o = off
        for _ in range(count):
            l = struct.unpack_from("<i", data, o)[0]; o += 4
            if l < 0:
                s = data[o:o - 2 * l].decode("utf-16-le"); o += -2 * l
            else:
                s = data[o:o + l].decode("latin1"); o += l
            o += 4                      # two uint16 hashes
            names.append(s.rstrip("\0"))
        return names
    except Exception:
        return None

for p in sys.argv[1:]:
    data = open(p, "rb").read()
    names = name_table(data)
    source = "name table"
    if names is None:
        names = [m.group().decode() for m in re.finditer(rb'[\x20-\x7e]{3,}', data)]
        source = "byte scan (cooked or unparsed header; 1-2 char names missing)"
    names = set(names)
    keys = sorted(n for n in names if KEY_RE.match(n))
    ias  = sorted(n for n in names if n.startswith("IA_") or "/InputActions/" in n)
    mods = sorted(n for n in names if MOD_RE.match(n))
    print(f"=== {p} ({len(data)} bytes, {source}) ===")
    print("KEYS:", " ".join(keys))
    print("ACTIONS:", " ".join(ias))
    print("MODS/TRIGGERS:", " ".join(mods))
