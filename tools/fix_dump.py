#!/usr/bin/env python3
"""Fix up the image RE3-1999CabbyCodes dumps (DumpImage=1) for Ghidra.

    python3 tools/fix_dump.py --dump "$GAME_DIR/ResidentEvil3.dumped.exe" \
        --imports "$GAME_DIR/ResidentEvil3.imports.txt" [--out ghidra_proj/ResidentEvil3.fixed.exe]

The mod writes the unpacked 1999 image - its six original sections only, with
raw offsets == RVAs - but the PE headers are still Enigma's: the entry point is
the wrapper's, and the data directories point into sections the dump does not
have. Enigma also clears the game's .idata once it has bound the imports, so
the import names are gone from the image; what is left is the import address
table at the front of .rdata, full of bound addresses.

This clears every data directory, points the entry at the game's own code
(--oep, else the CRT startup found by its GetCommandLineA/GetStartupInfoA
calls), and - given the mod's import map, which says which export each bound
slot resolved to - rebuilds a real import directory in the free space of
.idata: one descriptor per DLL, name tables, and the slots put back into their
unbound form, so Ghidra names all ~170 API calls by itself. Pure stdlib.
"""
import argparse, struct, collections

IMAGE_BASE = 0x400000


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    opt_sz = struct.unpack_from("<H", d, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = opt + opt_sz + i * 40
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
        out.append((va, vsz, rptr, rsz))
    return pe, opt, out


def read_import_map(path):
    """[(slot RVA, "dll!Export")] in file order, skipping unresolved slots."""
    out = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) < 3 or "!" not in parts[2]:
            continue
        out.append((int(parts[0], 16), parts[2]))
    return out


def find_free_run(d, rva, size, need):
    """The offset of a run of `need` zero bytes inside [rva, rva+size)."""
    run = 0
    for i in range(size):
        if d[rva + i]:
            run = 0
        else:
            run += 1
            if run >= need:
                return rva + i - need + 1
    return None


def build_import_directory(d, secs, imports):
    """Write descriptors + name tables into the free tail of .idata. Returns its RVA."""
    groups = []  # [(dll, [(slot_rva, export)])] - consecutive slots of one DLL
    for slot, full in imports:
        dll, fn = full.split("!", 1)
        if groups and groups[-1][0].lower() == dll.lower() and groups[-1][1][-1][0] + 4 == slot:
            groups[-1][1].append((slot, fn))
        else:
            groups.append((dll, [(slot, fn)]))
    # Sizes: one 20-byte descriptor per group plus a null one; per group an
    # OriginalFirstThunk array (one dword per import plus a terminator); per
    # import a hint/name; per group the DLL's name.
    need = 20 * (len(groups) + 1)
    for dll, items in groups:
        need += 4 * (len(items) + 1) + len(dll) + 1
        for _, fn in items:
            need += 2 + len(fn) + 1 + ((len(fn) + 1) & 1)
    idata_rva, idata_size = secs[3][0], secs[3][1]
    start = find_free_run(d, idata_rva, idata_size, need + 64)
    if start is None:
        raise SystemExit(f".idata has no free run of {need} bytes for a rebuilt import directory")
    start = (start + 3) & ~3
    desc = start
    p = desc + 20 * (len(groups) + 1)

    def put(b):
        nonlocal p
        d[p:p + len(b)] = b
        at = p
        p += len(b)
        return at

    for i, (dll, items) in enumerate(groups):
        name_rva = put(dll.encode() + b"\0")
        p = (p + 1) & ~1
        thunks = []
        for _, fn in items:
            thunks.append(put(struct.pack("<H", 0) + fn.encode() + b"\0"))
            p = (p + 1) & ~1
        p = (p + 3) & ~3
        oft = p
        for t in thunks:
            put(struct.pack("<I", t))
        put(struct.pack("<I", 0))
        struct.pack_into("<IIIII", d, desc + 20 * i, oft, 0, 0, name_rva, items[0][0])
        # Unbind the slots: a bound table would make Ghidra read addresses as names.
        for (slot, _), t in zip(items, thunks):
            struct.pack_into("<I", d, slot, t)
    struct.pack_into("<IIIII", d, desc + 20 * len(groups), 0, 0, 0, 0, 0)
    return desc, 20 * (len(groups) + 1), groups


def find_crt_entry(d, secs, imports):
    """The CRT startup: the one function whose calls include GetCommandLineA and GetStartupInfoA."""
    want = {}
    for slot, full in imports:
        fn = full.split("!", 1)[1]
        if fn in ("GetCommandLineA", "GetStartupInfoA", "GetModuleHandleA", "GetVersion"):
            want.setdefault(fn, slot + IMAGE_BASE)
    if "GetCommandLineA" not in want or "GetStartupInfoA" not in want:
        return None
    text_rva, text_size = secs[0][0], secs[0][1]
    hits = collections.defaultdict(set)
    for i in range(text_size - 6):
        o = text_rva + i
        if d[o] == 0xFF and d[o + 1] == 0x15:
            a = struct.unpack_from("<I", d, o + 2)[0]
            for fn, slot in want.items():
                if a == slot:
                    hits[fn].add(IMAGE_BASE + o)
    cmd, sui = hits.get("GetCommandLineA", set()), hits.get("GetStartupInfoA", set())
    if not cmd or not sui:
        return None
    # The startup routine calls both within a few hundred bytes of each other.
    best = None
    for a in cmd:
        for b in sui:
            if abs(a - b) < 0x300:
                lo = min(a, b)
                if best is None or lo < best:
                    best = lo
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--imports", help="the mod's ResidentEvil3.imports.txt from the same run")
    ap.add_argument("--out")
    ap.add_argument("--oep", type=lambda s: int(s, 0), help="entry point VA (default: the CRT startup, found)")
    a = ap.parse_args()
    d = bytearray(open(a.dump, "rb").read())
    pe, opt, secs = sections(d)
    if len(secs) != 6:
        raise SystemExit(f"expected the mod's 6-section dump, found {len(secs)} sections")
    ndirs = struct.unpack_from("<I", d, opt + 92)[0]
    for i in range(min(ndirs, 16)):
        struct.pack_into("<II", d, opt + 96 + i * 8, 0, 0)

    imports = read_import_map(a.imports) if a.imports else []
    if imports:
        rva, size, groups = build_import_directory(d, secs, imports)
        struct.pack_into("<II", d, opt + 96 + 8, rva, size)
        print(f"import directory: {len(groups)} DLLs, {len(imports)} imports, rebuilt at RVA {rva:#x}")
        for dll, items in groups:
            print(f"  {dll}: {len(items)} at {IMAGE_BASE + items[0][0]:#x}")
    else:
        print("import directory: no --imports given - API calls stay unnamed")

    oep = a.oep if a.oep else find_crt_entry(d, secs, imports)
    if oep is None:
        oep = IMAGE_BASE + secs[0][0]
        print("entry point: the CRT startup was not found - using the start of .text")
    oep_rva = oep - IMAGE_BASE if oep >= IMAGE_BASE else oep
    struct.pack_into("<I", d, opt + 16, oep_rva)

    out = a.out or a.dump.rsplit(".", 1)[0].replace(".dumped", "") + ".fixed.exe"
    open(out, "wb").write(d)
    print(f"wrote {out}: entry point RVA {oep_rva:#x} (VA {IMAGE_BASE + oep_rva:#x}); sections:")
    for va, vsz, rptr, rsz in secs:
        print(f"  va={IMAGE_BASE + va:#x} vsize={vsz:#x} raw={rptr:#x} rsize={rsz:#x}")


if __name__ == "__main__":
    main()
