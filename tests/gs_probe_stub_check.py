# Verifies the machine code InstallGsFailureProbe emits for __security_check_cookie (RVA 0x7882F).
#
# That stub sits on the return path of 275 functions. If its fast path were wrong -- a branch
# inverted, a byte miscounted -- every protected function in the game would take the failure path
# and nothing would run at all. Cheap to check, expensive to get wrong.
#
# Keep this in step with InstallGsFailureProbe in src/trace_hooks.cpp.
#
#   py -3 tests/gs_probe_stub_check.py       (needs capstone)
import struct
import sys

try:
    import capstone
except ImportError:
    print("capstone not installed; skipping")
    sys.exit(0)

BASE, REPORT, FAILJMP = 0x05970000, 0x06000000, 0x478838

b = bytearray()
b += bytes([0x3B, 0x0D, 0xA0, 0xFC, 0x4A, 0x00])          # cmp ecx, [0x4AFCA0]
b += bytes([0x75, 0x01])                                   # jne +1
b += bytes([0xC3])                                         # ret
b += bytes([0x60])                                         # pushad
b += bytes([0x8D, 0x44, 0x24, 0x20])                       # lea eax, [esp+0x20]
b += bytes([0x50])                                         # push eax
b += bytes([0xE8]) + struct.pack("<i", REPORT - (BASE + len(b) + 5))
b += bytes([0x83, 0xC4, 0x04])                             # add esp, 4
b += bytes([0x61])                                         # popad
b += bytes([0xE9]) + struct.pack("<i", FAILJMP - (BASE + len(b) + 5))

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
ins = list(md.disasm(bytes(b), BASE))
for i in ins:
    print("  %08X  %-8s %s" % (i.address, i.mnemonic, i.op_str))

at = {i.address - BASE: i for i in ins}
failures = []


def check(cond, what):
    print(("  OK   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


print()
check(ins[0].mnemonic == "cmp" and "0x4afca0" in ins[0].op_str,
      "fast path opens with the original compare")
check(at[6].mnemonic == "jne", "branch is jne, so it is taken only on MISMATCH")
check(int(at[6].op_str, 16) - BASE == 9, "mismatch jumps to offset 9 (pushad)")
check(8 in at and at[8].mnemonic == "ret",
      "match falls through to ret at offset 8 -- byte-identical to the original")
check(at[10].mnemonic == "lea" and at[10].op_str == "eax, [esp + 0x20]",
      "lea recovers the entry esp after pushad, so [eax] is the smashed return address")
check(ins[-1].mnemonic == "jmp" and int(ins[-1].op_str, 16) == FAILJMP,
      "tail jumps into the game's own failure path")
check(len(b) <= 48, "stub fits the 48-byte allocation (%d bytes)" % len(b))

print()
print("gs probe stub: ok" if not failures else "GS PROBE STUB BROKEN")
sys.exit(1 if failures else 0)
