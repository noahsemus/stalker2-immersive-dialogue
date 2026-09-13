import re, sys, struct
for p in sys.argv[1:]:
    data = open(p, "rb").read()
    names = set(m.group().decode() for m in re.finditer(rb'[\x20-\x7e]{3,}', data))
    keys = sorted(n for n in names if re.match(r'^(Gamepad_|Mouse|LeftMouse|RightMouse|MiddleMouse|[A-Z]$|Up$|Down$|Left$|Right$|Space|Escape|Enter|Tab|Left(Shift|Control|Alt)|Right(Shift|Control|Alt)|F\d+$|Zero|One|Two|Three|Four|Five|Six|Seven|Eight|Nine|Thumb|Caps|BackSpace|Delete|Insert|Home|End|PageUp|PageDown|NumPad)', n))
    ias  = sorted(n for n in names if n.startswith("IA_") or "/InputActions/" in n)
    mods = sorted(n for n in names if re.match(r'^(InputModifier|InputTrigger|Swizzle|Negate|DeadZone|Scalar|SmoothDelta|Pressed|Released|Hold|Tap)', n))
    print(f"=== {p} ({len(data)} bytes) ===")
    print("KEYS:", " ".join(keys))
    print("ACTIONS:", " ".join(ias))
    print("MODS/TRIGGERS:", " ".join(mods))
