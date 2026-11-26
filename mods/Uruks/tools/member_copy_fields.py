"""Audit the by-value copy metadata against a local Steam 1.21 executable.

Usage: python member_copy_fields.py path/to/ShadowOfWar.exe [--emit]
Requires pefile and capstone. No process is opened and no files are modified.
"""
import argparse
import hashlib
from pathlib import Path
import re

import capstone as cs
from capstone import x86_const as x
import pefile


def native_fields(executable):
    pe = pefile.PE(str(executable), fast_load=True)
    code = pe.get_data(0x1BB936C, 0x551)
    pe.close()
    if hashlib.sha256(code).hexdigest() != "630eebf2266152eb1d48aa81d705f65f39aff4599475d800a233201fc811a43e":
        raise ValueError("unrecognized native member copy routine")
    decoder = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64)
    decoder.detail = True
    loads, fields = {}, []
    for ins in decoder.disasm(code, 0x141BB936C):
        if not ins.mnemonic.startswith("mov") or len(ins.operands) != 2:
            continue
        dst, src = ins.operands
        if dst.type == x.X86_OP_REG:
            loads[ins.reg_name(dst.reg)] = (
                (src.mem.disp, src.size) if src.type == x.X86_OP_MEM and src.mem.base == x.X86_REG_RBX else None
            )
        elif dst.type == x.X86_OP_MEM and dst.mem.base == x.X86_REG_RDI and src.type == x.X86_OP_REG:
            reg = ins.reg_name(src.reg)
            field = (dst.mem.disp, dst.size)
            if loads.get("eax" if reg == "ax" else reg) != field:
                raise ValueError(f"unclassified destination store at {ins.address:x}")
            fields.append(field)
    return sorted(fields)


if __name__ == "__main__":
    args = argparse.ArgumentParser()
    args.add_argument("executable", type=Path)
    args.add_argument("--emit", action="store_true")
    opts = args.parse_args()
    fields = native_fields(opts.executable)
    if opts.emit:
        print(",\n".join(f"{{0x{offset:x},{size}}}" for offset, size in fields))
    else:
        header = (Path(__file__).resolve().parents[1] / "src/MemberCopy.h").read_text()
        table = header.split("Region nativeValues[]={", 1)[1].split("};", 1)[0]
        compiled = [(int(off, 16), int(size)) for off, size in re.findall(r"\{0x([0-9a-f]+),(\d+)\}", table)]
        if compiled != fields:
            raise SystemExit("member copy metadata differs; inspect --emit")
        print(f"Verified all {len(fields)} native value assignments")
