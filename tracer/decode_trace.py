#!/usr/bin/env python3
# Decode ez2trace.bin (stream of 4-byte EIP RVAs) into readable execution behavior.
#
#   python tracer/decode_trace.py ez2trace.bin            # function-sequence (call flow) + summary
#   python tracer/decode_trace.py ez2trace.bin --summary  # only the hot-function summary
#   python tracer/decode_trace.py ez2trace.bin --full     # every instruction VA (huge)
#
# Function names/boundaries are read from decomp/decompiled_all.c (Ghidra FUN_<VA> labels), so the
# output reads as the sequence of game functions executed — the "internal behaviour" you want to see.
import sys, os, re, struct, bisect, collections

IMGBASE = 0x400000

def load_funcs():
    # collect FUN_00xxxxxx labels from the decomp as function-start anchors
    for cand in ("decomp/decompiled_all.c", "../decomp/decompiled_all.c",
                 os.path.join(os.path.dirname(__file__), "..", "decomp", "decompiled_all.c")):
        if os.path.exists(cand):
            txt = open(cand, "r", errors="ignore").read()
            vas = sorted(set(int(h, 16) for h in re.findall(r"FUN_([0-9a-fA-F]{8})", txt)))
            return vas
    return []

def name(va, starts):
    if not starts:
        return "sub_%06x" % va
    i = bisect.bisect_right(starts, va) - 1
    base = starts[i] if i >= 0 else va
    off = va - base
    return "FUN_%08x" % base + (("+0x%x" % off) if off else "")

def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    path = sys.argv[1]
    mode = "flow"
    if "--summary" in sys.argv: mode = "summary"
    if "--full" in sys.argv: mode = "full"

    data = open(path, "rb").read()
    n = len(data) // 4
    rvas = struct.unpack_from("<%dI" % n, data, 0)
    starts = load_funcs()
    print("# %d instructions traced, %d known functions" % (n, len(starts)), file=sys.stderr)

    if mode == "full":
        for r in rvas:
            print("%08x" % (IMGBASE + r))
        return

    # collapse the instruction stream into consecutive runs within the same function
    counts = collections.Counter()   # func-start VA -> total instructions
    visits = collections.Counter()   # func-start VA -> times entered
    flow = []                        # (func-start VA, instrs-in-this-visit)
    cur = None; run = 0
    for r in rvas:
        va = IMGBASE + r
        i = bisect.bisect_right(starts, va) - 1
        f = starts[i] if (starts and i >= 0) else va
        counts[f] += 1
        if f == cur:
            run += 1
        else:
            if cur is not None:
                flow.append((cur, run)); visits[cur] += 1
            cur = f; run = 1
    if cur is not None:
        flow.append((cur, run)); visits[cur] += 1

    if mode == "flow":
        for f, run in flow:
            print("%-22s x%d" % (name(f, starts), run))
    print("\n==== hot functions (by instructions executed) ====", file=sys.stderr)
    for f, c in counts.most_common(40):
        print("%-22s instrs=%-9d visits=%d" % (name(f, starts), c, visits[f]), file=sys.stderr)

if __name__ == "__main__":
    main()
