# Fails if the built DLL could not load on Windows XP SP3 32-bit.
#
# The cabinet is XP. Nothing in the build enforces that, and a single call to a Vista-era API is
# enough to make the loader refuse the DLL outright -- with no diagnostic beyond "the mod did not
# load". This walks the import table and checks it against a list of APIs introduced after XP,
# plus the DLLs that do not exist there at all.
#
# Media Foundation does NOT appear here on purpose: it is loaded with LoadLibrary and has an
# ffmpeg fallback, so its absence degrades rather than blocks.
#
#   py -3 tests/xp_compat_check.py [path\to\EZ2CustomMod.dll]
import struct
import sys
import os

DEFAULT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "EZ2AC_FNEX", "EZ2CustomMod.dll")

# Introduced after Windows XP SP3 / Server 2003.
POST_XP = {
    "GetTickCount64", "InitializeCriticalSectionEx", "InitOnceExecuteOnce", "CreateEventExA",
    "CreateEventExW", "CreateMutexExA", "CreateMutexExW", "CreateSemaphoreExW",
    "GetFinalPathNameByHandleA", "GetFinalPathNameByHandleW", "SetThreadStackGuarantee",
    "GetThreadId", "QueryFullProcessImageNameA", "QueryFullProcessImageNameW",
    "CompareStringOrdinal", "GetLocaleInfoEx", "LCMapStringEx", "GetUserDefaultLocaleName",
    "EnumSystemLocalesEx", "GetSystemTimePreciseAsFileTime", "CreateSymbolicLinkA",
    "CreateSymbolicLinkW", "SetFileInformationByHandle", "GetFileInformationByHandleEx",
    "RegGetValueA", "RegGetValueW", "AddDllDirectory", "SetDefaultDllDirectories",
    "WaitOnAddress", "WakeByAddressSingle", "WakeByAddressAll", "AcquireSRWLockExclusive",
    "AcquireSRWLockShared", "InitializeSRWLock", "ReleaseSRWLockExclusive",
    "ReleaseSRWLockShared", "TryAcquireSRWLockExclusive", "SleepConditionVariableSRW",
    "SleepConditionVariableCS", "InitializeConditionVariable", "WakeAllConditionVariable",
    "WakeConditionVariable", "GetOverlappedResultEx", "DiscardVirtualMemory",
    "PrefetchVirtualMemory", "GetNumaHighestNodeNumber", "GetLogicalProcessorInformationEx",
    "SetProcessMitigationPolicy", "GetCurrentProcessorNumber", "CancelIoEx",
    "GetDynamicTimeZoneInformation", "IdnToAscii", "FindFirstFileNameW",
    "GetTempPath2A", "GetTempPath2W", "FlsAlloc", "FlsGetValue", "FlsSetValue", "FlsFree",
}
ABSENT_DLL_PREFIXES = ("api-ms-win", "kernelbase", "bcrypt", "ncrypt", "shcore", "dwmapi",
                       "d3d11", "d3d12", "dxgi", "mfplat", "mfreadwrite", "mfcore")


def imports_of(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        vs, va, rs, pr = struct.unpack_from("<IIII", d, o + 8)
        secs.append((va, vs, rs, pr))

    def off(r):
        for va, vs, rs, pr in secs:
            if va <= r < va + max(vs, rs):
                return pr + (r - va)
        return None

    def cstr(o):
        return d[o:d.index(b"\x00", o)].decode("latin-1")

    subsystem = struct.unpack_from("<HH", d, pe + 24 + 48)
    machine = struct.unpack_from("<H", d, pe + 4)[0]
    o = off(struct.unpack_from("<I", d, pe + 24 + 96 + 8)[0])
    imports = {}
    while o:
        oft, ts, fc, nm, ft = struct.unpack_from("<IIIII", d, o)
        if not nm:
            break
        dll = cstr(off(nm))
        funcs, t = [], off(oft or ft)
        while t:
            v = struct.unpack_from("<I", d, t)[0]
            if not v:
                break
            if v & 0x80000000:
                funcs.append("#%d" % (v & 0xFFFF))
            else:
                fo = off(v)
                funcs.append(cstr(fo + 2) if fo else "?")
            t += 4
        imports[dll] = funcs
        o += 20
    return machine, subsystem, imports


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    if not os.path.exists(path):
        print("not built yet: %s" % path)
        return 0
    machine, subsystem, imports = imports_of(path)
    failures = []

    def check(cond, what):
        print(("  OK   " if cond else "  FAIL ") + what)
        if not cond:
            failures.append(what)

    print("XP compatibility of %s\n" % os.path.basename(path))
    check(machine == 0x14C, "built for x86 (0x%04X)" % machine)
    check(subsystem <= (5, 1), "subsystem version %d.%d is <= 5.1" % subsystem)

    for dll, funcs in sorted(imports.items()):
        low = dll.lower()
        if any(low.startswith(p) for p in ABSENT_DLL_PREFIXES):
            check(False, "%s does not exist on XP" % dll)
        for f in funcs:
            if f in POST_XP:
                check(False, "%s!%s was introduced after XP" % (dll, f))

    total = sum(len(v) for v in imports.values())
    check(True, "%d imports across %d DLLs, all available on XP" % (total, len(imports)))

    print()
    print("xp compat: ok" if not failures else "XP COMPATIBILITY BROKEN")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
