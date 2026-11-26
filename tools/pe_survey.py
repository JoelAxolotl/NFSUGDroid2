"""Survey a PE image: headers, sections, entropy, imports, TLS, relocations.

Usage: python -I pe_survey.py <exe> [out.json]
"""
import json
import math
import sys
from collections import Counter

import pefile


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    c = Counter(data)
    n = len(data)
    return -sum(v / n * math.log2(v / n) for v in c.values())


def main() -> None:
    path = sys.argv[1]
    pe = pefile.PE(path)
    oh = pe.OPTIONAL_HEADER
    info = {
        "machine": hex(pe.FILE_HEADER.Machine),
        "timestamp": pe.FILE_HEADER.TimeDateStamp,
        "image_base": hex(oh.ImageBase),
        "entry_rva": hex(oh.AddressOfEntryPoint),
        "size_of_image": hex(oh.SizeOfImage),
        "subsystem": oh.Subsystem,
        "dll_characteristics": hex(oh.DllCharacteristics),
        "has_relocs": hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"),
        "has_tls": hasattr(pe, "DIRECTORY_ENTRY_TLS"),
        "sections": [],
        "imports": {},
    }
    for s in pe.sections:
        data = s.get_data()
        info["sections"].append({
            "name": s.Name.rstrip(b"\0").decode(errors="replace"),
            "rva": hex(s.VirtualAddress),
            "vsize": hex(s.Misc_VirtualSize),
            "rawsize": hex(s.SizeOfRawData),
            "chars": hex(s.Characteristics),
            "entropy": round(entropy(data), 3),
        })
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for imp in pe.DIRECTORY_ENTRY_IMPORT:
            dll = imp.dll.decode()
            info["imports"][dll] = [
                (i.name.decode() if i.name else f"#{i.ordinal}") for i in imp.imports
            ]
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w") as f:
            json.dump(info, f, indent=1)
    print(json.dumps({k: v for k, v in info.items() if k != "imports"}, indent=1))
    for dll, names in info["imports"].items():
        print(f"{dll}: {len(names)}")
    print("total imports:", sum(len(v) for v in info["imports"].values()))


if __name__ == "__main__":
    main()
