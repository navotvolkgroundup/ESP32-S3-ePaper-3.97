#!/usr/bin/env python3
"""Every code pc_button_classify() matches must be emitted, and unambiguously.

Two failure modes, both of which really happened here:

DEAD ARM -- the code is never emitted, so the press does nothing and looks
like a flat battery or a wedged panel. PC_BTN_SETTINGS was written as
BTN_BOOT_BASE + EV_LONG_HOLD (27); button_bsp.c's LONG_PRESS_HOLD arm for
Boot_id sets bit 23 instead, so 27 never arrives and the wake log could not
be opened at all.

AMBIGUOUS CODE -- one bit is set by more than one event, so matching it means
matching all of them. Bit 23 is Boot PRESS_DOWN *and* Boot LONG_PRESS_START
*and* Boot LONG_PRESS_HOLD, so mapping the wake log to 23 would have opened
it on every ordinary Boot click. The fix was 26, which only LONG_PRESS_START
for Boot sets.

Both files compile perfectly well while disagreeing, which is why this is a
cross-artifact check and not a unit test.
"""
import re, sys

# code -> set of "Button/EVENT" that emit it
emitted = {}
ev = btn = None
for line in open('components/button_bsp/button_bsp.c'):
    m = re.search(r'case\s+(SINGLE_CLICK|DOUBLE_CLICK|PRESS_DOWN|PRESS_UP|'
                  r'PRESS_REPEAT|LONG_PRESS_START|LONG_PRESS_HOLD)\s*:', line)
    if m: ev = m.group(1); continue
    b = re.search(r'case\s+(Button_\w+_id|Boot_id)\s*:', line)
    if b: btn = b.group(1); continue
    n = re.search(r'set_bit_button\((\d+)\)', line)
    if n: emitted.setdefault(int(n.group(1)), set()).add(f"{btn}/{ev}")

src = open('main/page_common/page_common.cc').read()
consts = {m.group(1): int(m.group(2))
          for m in re.finditer(r'\b(BTN_[A-Z_]+|EV_[A-Z_]+)\s*=\s*(\d+)', src)}
body = re.search(r'pc_button_classify\(int code\)\s*\{.*?\n\}', src, re.S)
if not body:
    sys.exit("could not find pc_button_classify()")

bad = []
seen = 0
for m in re.finditer(r'case\s+([A-Z_0-9\s+]+?):\s*return\s+(PC_BTN_\w+)', body.group(0)):
    expr, meaning = m.group(1).strip(), m.group(2)
    try:
        val = eval(expr, {"__builtins__": {}}, consts)
    except Exception:
        sys.exit(f"could not evaluate case '{expr}'")
    seen += 1
    sources = emitted.get(val)
    if not sources:
        bad.append(f"DEAD ARM: code {val} -> {meaning} is never emitted")
    elif len(sources) > 1:
        bad.append(f"AMBIGUOUS: code {val} -> {meaning} is set by "
                   f"{', '.join(sorted(sources))}")

if bad:
    print('\n'.join("  " + b for b in bad))
    sys.exit(1)
print(f"button check OK: {seen} classified codes, all emitted and unambiguous")
