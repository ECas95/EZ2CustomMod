"""Decrypt / re-encrypt EZ2AC's Ez2ac.ini, mirroring FnexEncryptBuffer in src/fnex_crypto.cpp."""
import re
import sys

SRC = r"C:\EZ2AC\EZ2CustomMod_v25_category_gate_silent_load_fix\src\fnex_crypto.cpp"


def load_key():
    s = open(SRC, encoding="utf-8", errors="surrogateescape").read()
    i = s.index("kFnexIniKey[0x200] = {")
    body = s[i + len("kFnexIniKey[0x200] = {"): s.index("};", i)]
    key = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]{2}|\b\d+\b", body)]
    assert len(key) == 0x200, "key came out %d bytes" % len(key)
    return key


def blocks(size):
    """The exact block walk from FnexEncryptBuffer."""
    off, key_index, wide = 0, 0, False
    while off < size:
        if size < off + 8:
            ln = size - off
        else:
            if wide:
                ln = 16
                if size < off + 16:
                    ln = size - off
            else:
                ln = 8
            wide = not wide
        yield off, ln, key_index
        key_index += ln
        if key_index > 0x1FF:
            key_index -= 0x200
        if ln > 8:
            off = (off - 8) + ln
        off += 8


def encrypt(data, key):
    size = len(data)
    tmp = bytearray(size)
    for off, ln, ki in blocks(size):
        for u in range(ln):
            k = ki + ln - u - 1
            if k > 0x1FF:
                k -= 0x200
            tmp[off + ln - 1 - u] = (data[off + u] + key[k & 0x1FF]) & 0xFF
    tmp.reverse()
    return bytes(tmp)


def decrypt(blob, key):
    size = len(blob)
    tmp = bytearray(blob)
    tmp.reverse()                       # undo the whole-buffer reverse
    out = bytearray(size)
    for off, ln, ki in blocks(size):
        for u in range(ln):
            k = ki + ln - u - 1
            if k > 0x1FF:
                k -= 0x200
            out[off + u] = (tmp[off + ln - 1 - u] - key[k & 0x1FF]) & 0xFF
    return bytes(out)


if __name__ == "__main__":
    key = load_key()
    mode, path = sys.argv[1], sys.argv[2]
    blob = open(path, "rb").read()
    if mode == "dec":
        out = decrypt(blob, key)
        # round-trip proof: re-encrypting must reproduce the file byte for byte
        assert encrypt(out, key) == blob, "round-trip FAILED -- do not trust this output"
        sys.stdout.buffer.write(out)
    elif mode == "enc":
        out = encrypt(blob, key)
        assert decrypt(out, key) == blob, "round-trip FAILED"
        sys.stdout.buffer.write(out)
