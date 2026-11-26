"""Approximate x86 instruction census of a PE's .text by recursive descent
from the entry point plus every relocation target that lands in .text.

The relocation table of SPEED2.EXE survives in an unnamed section even though
the header's reloc directory was cleared; pass its RVA with --reloc-rva.

Usage: python -I isa_census.py <exe> --reloc-rva 0x4f5000 [--out census.json]
"""
import argparse
import json
import struct
from collections import Counter

import capstone
import pefile


def parse_relocs(pe: pefile.PE, rva: int, size: int) -> list[int]:
    """Return the RVAs of every HIGHLOW (type 3) relocation."""
    data = pe.get_data(rva, size)
    out = []
    off = 0
    while off + 8 <= len(data):
        page, blk = struct.unpack_from("<II", data, off)
        if blk < 8 or page == 0:
            break
        for i in range((blk - 8) // 2):
            (e,) = struct.unpack_from("<H", data, off + 8 + i * 2)
            if e >> 12 == 3:
                out.append(page + (e & 0xFFF))
        off += blk
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--reloc-rva", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--out")
    a = ap.parse_args()

    pe = pefile.PE(a.exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.startswith(b".text"))
    t0 = base + text.VirtualAddress
    t1 = t0 + text.Misc_VirtualSize
    tdata = text.get_data()
    rsec = next(s for s in pe.sections if s.VirtualAddress == a.reloc_rva)
    relocs = parse_relocs(pe, a.reloc_rva, rsec.Misc_VirtualSize)

    # Pointers stored at relocation sites that point into .text are code
    # candidates (vtables, function pointers, jump tables, call targets).
    seeds = {base + pe.OPTIONAL_HEADER.AddressOfEntryPoint}
    reloc_in_text = 0
    for r in relocs:
        try:
            (v,) = struct.unpack("<I", pe.get_data(r, 4))
        except Exception:
            continue
        if t0 <= v < t1:
            seeds.add(v)
        if t0 <= base + r < t1:
            reloc_in_text += 1

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    seen: set[int] = set()
    work = list(seeds)
    mnem = Counter()
    groups = Counter()
    bad = 0
    while work:
        addr = work.pop()
        while t0 <= addr < t1 and addr not in seen:
            off = addr - t0
            ins = next(md.disasm(tdata[off:off + 16], addr), None)
            if ins is None:
                bad += 1
                break
            seen.add(addr)
            mnem[ins.mnemonic] += 1
            for g in ins.groups:
                groups[ins.group_name(g)] += 1
            if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL):
                op = ins.operands[0] if ins.operands else None
                if op is not None and op.type == capstone.x86.X86_OP_IMM:
                    work.append(op.imm)
            if ins.mnemonic in ("ret", "retf", "jmp", "int3", "hlt", "ud2"):
                break
            addr += ins.size

    fpu = {k: v for k, v in mnem.items() if k.startswith("f")}
    simd_groups = {k: v for k, v in groups.items()
                   if k in ("sse1", "sse2", "sse3", "ssse3", "sse41", "mmx", "3dnow")}
    res = {
        "relocs": len(relocs),
        "relocs_in_text": reloc_in_text,
        "seeds": len(seeds),
        "decoded_insns": len(seen),
        "decode_failures": bad,
        "distinct_mnemonics": len(mnem),
        "simd_groups": simd_groups,
        "x87_insns": sum(fpu.values()),
        "top_mnemonics": mnem.most_common(60),
        "all_mnemonics": dict(mnem),
    }
    if a.out:
        with open(a.out, "w") as f:
            json.dump(res, f, indent=1)
    res.pop("all_mnemonics")
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
