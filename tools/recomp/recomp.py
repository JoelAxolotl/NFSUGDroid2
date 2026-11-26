"""Ahead-of-time x86-32 -> C++ translator for SPEED2.EXE.

Discovers guest functions statically and emits one C++ function per guest
function into android/app/src/main/cpp/generated/. The generated code runs on
cpu::Cpu and shares instruction semantics with the interpreter (cpu/ops.h,
cpu/x87_ops.h): anything the emitter does not translate natively is executed by
cpu::step_one, and control flow it cannot follow is handed to the dispatcher by
returning with EIP set, so translated and interpreted code can mix freely.

Usage:
  python -I tools/recomp/recomp.py <SPEED2.EXE> --reloc-rva 0x4f5000
         [--profile aot_profile.txt] [--out android/app/src/main/cpp/generated]
         [--per-file 400] [--only-profile]
"""
import argparse
import os
import struct
import sys
from collections import defaultdict

import capstone
import pefile
from capstone import x86 as X

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
REG16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
REG8L = ["al", "cl", "dl", "bl"]
REG8H = ["ah", "ch", "dh", "bh"]

CC = {"o": 0, "no": 1, "b": 2, "ae": 3, "e": 4, "ne": 5, "be": 6, "a": 7, "s": 8, "ns": 9, "p": 10,
      "np": 11, "l": 12, "ge": 13, "le": 14, "g": 15}
ALU = {"add": 0, "or": 1, "adc": 2, "sbb": 3, "and": 4, "sub": 5, "xor": 6, "cmp": 7}
SHIFT = {"rol": 0, "ror": 1, "rcl": 2, "rcr": 3, "shl": 4, "sal": 4, "shr": 5, "sar": 7}
MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}


def reg_info(name):
    if name in REG32:
        return REG32.index(name), 4, False
    if name in REG16:
        return REG16.index(name), 2, False
    if name in REG8L:
        return REG8L.index(name), 1, False
    if name in REG8H:
        return REG8H.index(name), 1, True
    return None


class Image:
    def __init__(self, path, reloc_rva):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.entry = self.base + self.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        text = next(s for s in self.pe.sections if s.Name.startswith(b".text"))
        self.t0 = self.base + text.VirtualAddress
        self.t1 = self.t0 + text.Misc_VirtualSize
        self.tdata = text.get_data()
        rsec = next(s for s in self.pe.sections if s.VirtualAddress == reloc_rva)
        self.reloc_sites = set()
        data = self.pe.get_data(reloc_rva, rsec.Misc_VirtualSize)
        off = 0
        while off + 8 <= len(data):
            page, blk = struct.unpack_from("<II", data, off)
            if blk < 8 or page == 0:
                break
            for i in range((blk - 8) // 2):
                (e,) = struct.unpack_from("<H", data, off + 8 + i * 2)
                if e >> 12 == 3:
                    self.reloc_sites.add(self.base + page + (e & 0xFFF))
            off += blk

    def in_text(self, a):
        return self.t0 <= a < self.t1

    def rd32(self, va):
        try:
            return struct.unpack("<I", self.pe.get_data(va - self.base, 4))[0]
        except Exception:
            return None


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}          # addr -> capstone insn
        self.leaders = {entry}
        self.switches = {}       # jmp addr -> [targets]
        self.ok = True


class Recomp:
    def __init__(self, img):
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.cache = {}
        self.funcs = {}
        self.entries = set()

    def decode(self, a):
        if a in self.cache:
            return self.cache[a]
        img = self.img
        if not img.in_text(a):
            return None
        off = a - img.t0
        ins = next(self.md.disasm(img.tdata[off:off + 16], a), None)
        self.cache[a] = ins
        return ins

    def jump_table(self, ins):
        """Targets of `jmp [index*4 + table]` when the table is relocated data."""
        op = ins.operands[0]
        if op.type != X.X86_OP_MEM or op.mem.index == 0 or op.mem.scale != 4 or op.mem.base != 0:
            return None
        table = op.mem.disp & 0xFFFFFFFF
        targets = []
        a = table
        while a in self.img.reloc_sites and len(targets) < 1024:
            v = self.img.rd32(a)
            if v is None or not self.img.in_text(v):
                break
            targets.append(v)
            a += 4
        return targets or None

    def explore(self, entry):
        f = Func(entry)
        work = [entry]
        new_calls = set()
        while work:
            a = work.pop()
            while True:
                if a in f.insns:
                    break
                ins = self.decode(a)
                if ins is None:
                    f.ok = False
                    break
                f.insns[a] = ins
                m = ins.mnemonic
                nxt = a + ins.size
                # Function pointers in immediates (push offset cb / mov reg, offset fn).
                for op in ins.operands:
                    if op.type == X.X86_OP_IMM:
                        v = op.imm & 0xFFFFFFFF
                        if self.img.in_text(v) and any(s in self.img.reloc_sites for s in range(a, nxt)):
                            if m != "call" and not m.startswith("j"):
                                new_calls.add(v)
                if m == "call":
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        new_calls.add(op.imm & 0xFFFFFFFF)
                    a = nxt
                    continue
                if m in ("ret", "retf", "iretd", "hlt", "ud2", "int3"):
                    break
                if m == "jmp":
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        f.leaders.add(t)
                        a = t
                        continue
                    tbl = self.jump_table(ins)
                    if tbl:
                        f.switches[a] = tbl
                        for t in tbl:
                            f.leaders.add(t)
                            work.append(t)
                    break
                if m.startswith("j") or m in ("loop", "loope", "loopne", "jecxz"):
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        f.leaders.add(t)
                        work.append(t)
                    f.leaders.add(nxt)
                    a = nxt
                    continue
                a = nxt
        return f, new_calls

    def discover(self, seeds):
        work = list(seeds)
        while work:
            e = work.pop()
            if e in self.funcs or not self.img.in_text(e):
                continue
            f, calls = self.explore(e)
            self.funcs[e] = f
            for t in calls:
                if t not in self.funcs:
                    work.append(t)


# --------------------------------------------------------------------------- emission
class Emitter:
    def __init__(self, rc, f):
        self.rc = rc
        self.f = f
        self.out = []
        self.native = 0
        self.fallback = 0

    def L(self, s):
        self.out.append(s)

    # ---- operands
    def ea(self, op, seg=True):
        mm = op.mem
        parts = []
        if mm.base:
            parts.append("R[%d]" % REG32.index(self.rc.md.reg_name(mm.base)))
        if mm.index:
            r = "R[%d]" % REG32.index(self.rc.md.reg_name(mm.index))
            parts.append(r if mm.scale == 1 else "%s * %du" % (r, mm.scale))
        if mm.disp or not parts:
            parts.append("0x%Xu" % (mm.disp & 0xFFFFFFFF))
        if seg and mm.segment and self.rc.md.reg_name(mm.segment) == "fs":
            parts.append("c.fs_base")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def rd(self, op, size=None, ea_var=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            idx, sz, hi = reg_info(self.rc.md.reg_name(op.reg))
            if sz == 4:
                return "R[%d]" % idx
            if sz == 2:
                return "(R[%d] & 0xFFFFu)" % idx
            return "((R[%d] >> 8) & 0xFFu)" % idx if hi else "(R[%d] & 0xFFu)" % idx
        if op.type == X.X86_OP_IMM:
            return "0x%Xu" % (op.imm & {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[size])
        a = ea_var or self.ea(op)
        return {1: "RD8(%s)", 2: "RD16(%s)", 4: "RD32(%s)"}[size] % a

    def wr(self, op, v, ea_var=None):
        if op.type == X.X86_OP_REG:
            idx, sz, hi = reg_info(self.rc.md.reg_name(op.reg))
            if sz == 4:
                return "R[%d] = %s;" % (idx, v)
            if sz == 2:
                return "R[%d] = (R[%d] & 0xFFFF0000u) | ((%s) & 0xFFFFu);" % (idx, idx, v)
            if hi:
                return "R[%d] = (R[%d] & 0xFFFF00FFu) | (((%s) & 0xFFu) << 8);" % (idx, idx, v)
            return "R[%d] = (R[%d] & 0xFFFFFF00u) | ((%s) & 0xFFu);" % (idx, idx, v)
        a = ea_var or self.ea(op)
        return {1: "WR8(%s, %s);", 2: "WR16(%s, %s);", 4: "WR32(%s, %s);"}[op.size] % (a, v)

    def plain_regs_ok(self, ins):
        for op in ins.operands:
            if op.type == X.X86_OP_REG and reg_info(self.rc.md.reg_name(op.reg)) is None:
                return False
            if op.type == X.X86_OP_MEM:
                mm = op.mem
                for r in (mm.base, mm.index):
                    if r and self.rc.md.reg_name(r) not in REG32:
                        return False
                if mm.segment and self.rc.md.reg_name(mm.segment) not in ("fs", "ds", "ss", "es", "cs"):
                    return False
        return True

    def target_code(self, t):
        if t in self.f.insns:
            return "goto L_%08X;" % t
        return "{ c.eip = 0x%Xu; return; }" % t

    def fallback(self, ins, check_flow=True):
        self.fallback += 1
        nxt = ins.address + ins.size
        self.L("  c.eip = 0x%Xu; cpu::step_one(c); if (c.stop != cpu::Stop::None%s) return;" %
               (ins.address, (" || c.eip != 0x%Xu" % nxt) if check_flow else ""))

    def emit_call(self, ins, target_expr, direct_fn):
        ret = ins.address + ins.size
        self.L("  { uint32_t sp0 = R[4]; uint32_t t = %s; cpu::push32(c, 0x%Xu);" % (target_expr, ret))
        if direct_fn:
            self.L("    c.eip = t; %s(c);" % direct_fn)
            self.L("    if (c.eip != 0x%Xu || R[4] < sp0) { if (c.stop != cpu::Stop::None) return; "
                   "cpu::run_until(c, 0x%Xu, sp0); }" % (ret, ret))
        else:
            self.L("    aotrt::call(c, t, 0x%Xu, sp0);" % ret)
        self.L("    if (c.stop != cpu::Stop::None) return; }")

    def emit(self, ins):
        m = ins.mnemonic
        a = ins.address
        nxt = a + ins.size
        ops = ins.operands
        raw = bytes(ins.bytes)
        # Strip prefixes to find the primary opcode byte.
        p = 0
        prefixes = set()
        while p < len(raw) and raw[p] in (0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65):
            prefixes.add(raw[p])
            p += 1
        opc = raw[p] if p < len(raw) else 0

        if 0xF0 in prefixes or 0x67 in prefixes or not self.plain_regs_ok(ins):
            return self.fallback(ins)

        # x87: execute through the shared core with the pre-decoded address.
        if 0xD8 <= opc <= 0xDF and 0x66 not in prefixes:
            modrm = raw[p + 1]
            mem_op = next((o for o in ops if o.type == X.X86_OP_MEM), None)
            ea = self.ea(mem_op) if (modrm < 0xC0 and mem_op is not None) else "0u"
            self.L("  if (!cpu::x87_core(c, 0x%02X, 0x%02X, %s)) { c.eip = 0x%Xu; cpu::step_one(c); "
                   "if (c.stop != cpu::Stop::None) return; }" % (opc, modrm, ea, a))
            self.native += 1
            return

        # String instructions.
        if 0xA4 <= opc <= 0xAF and opc not in (0xA8, 0xA9):
            rep = 0xF3 if 0xF3 in prefixes else (0xF2 if 0xF2 in prefixes else 0)
            sz = 1 if opc % 2 == 0 else (2 if 0x66 in prefixes else 4)
            seg = "c.fs_base" if 0x64 in prefixes else "0u"
            self.L("  cpu::string_op(c, 0x%02X, %d, 0x%02X, %s);" % (opc, sz, rep, seg))
            self.native += 1
            return

        sz = ops[0].size if ops else 4
        if m in ALU and len(ops) == 2:
            op = ALU[m]
            self.L("  {")
            ea = None
            if ops[0].type == X.X86_OP_MEM:
                self.L("    uint32_t ea = %s;" % self.ea(ops[0]))
                ea = "ea"
            src = self.rd(ops[1], ops[0].size) if ops[1].type == X.X86_OP_IMM else self.rd(ops[1])
            if m == "xor" and ops[1].type == X.X86_OP_REG and ops[0].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                self.L("    uint32_t r = cpu::alu(c, 6, 0u, 0u, %d); %s" % (sz, self.wr(ops[0], "r")))
            elif op == 7:
                self.L("    cpu::alu(c, 7, %s, %s, %d);" % (self.rd(ops[0], ea_var=ea), src, sz))
            else:
                self.L("    uint32_t r = cpu::alu(c, %d, %s, %s, %d); %s" %
                       (op, self.rd(ops[0], ea_var=ea), src, sz, self.wr(ops[0], "r", ea)))
            self.L("  }")
        elif m == "test" and len(ops) == 2:
            self.L("  cpu::alu(c, 4, %s, %s, %d);" % (self.rd(ops[0]), self.rd(ops[1], ops[0].size), sz))
        elif m == "mov" and len(ops) == 2 and ops[0].size == ops[1].size or (m == "mov" and ops[1].type == X.X86_OP_IMM):
            if ops[0].type == X.X86_OP_REG and self.rc.md.reg_name(ops[0].reg) not in REG32 + REG16 + REG8L + REG8H:
                return self.fallback(ins)
            if ops[1].type == X.X86_OP_REG and reg_info(self.rc.md.reg_name(ops[1].reg)) is None:
                return self.fallback(ins)
            self.L("  " + self.wr(ops[0], self.rd(ops[1], ops[0].size)))
        elif m in ("movzx", "movsx"):
            v = self.rd(ops[1])
            if m == "movsx":
                v = "cpu::sext(%s, %d)" % (v, ops[1].size)
            self.L("  " + self.wr(ops[0], v))
        elif m == "lea":
            self.L("  " + self.wr(ops[0], self.ea(ops[1], seg=False)))
        elif m == "push" and sz == 4:
            self.L("  { uint32_t v = %s; cpu::push32(c, v); }" % self.rd(ops[0], 4))
        elif m == "pop" and sz == 4:
            self.L("  { uint32_t v = cpu::pop32(c); %s }" % self.wr(ops[0], "v"))
        elif m in ("inc", "dec"):
            self.L("  {")
            ea = None
            if ops[0].type == X.X86_OP_MEM:
                self.L("    uint32_t ea = %s;" % self.ea(ops[0]))
                ea = "ea"
            self.L("    uint32_t r = cpu::inc_dec(c, %s, %s, %d); %s" %
                   (self.rd(ops[0], ea_var=ea), "true" if m == "dec" else "false", sz, self.wr(ops[0], "r", ea)))
            self.L("  }")
        elif m in ("neg", "not"):
            self.L("  {")
            ea = None
            if ops[0].type == X.X86_OP_MEM:
                self.L("    uint32_t ea = %s;" % self.ea(ops[0]))
                ea = "ea"
            if m == "not":
                self.L("    %s" % self.wr(ops[0], "~%s" % self.rd(ops[0], ea_var=ea), ea))
            else:
                self.L("    uint32_t v = %s & %s; uint32_t r = (0u - v) & %s;" % (self.rd(ops[0], ea_var=ea), MASK[sz], MASK[sz]))
                self.L("    cpu::set_flags_lazy(c, cpu::LF_NEG, %d, 0, v, r); %s" % (sz, self.wr(ops[0], "r", ea)))
            self.L("  }")
        elif m in SHIFT:
            if len(ops) == 1:
                cnt = "1u"
            elif ops[1].type == X.X86_OP_IMM:
                cnt = "%du" % (ops[1].imm & 0xFF)
            else:
                cnt = "(R[1] & 0xFFu)"
            self.L("  {")
            ea = None
            if ops[0].type == X.X86_OP_MEM:
                self.L("    uint32_t ea = %s;" % self.ea(ops[0]))
                ea = "ea"
            self.L("    uint32_t r = cpu::shift(c, %d, %s, %s, %d); %s" %
                   (SHIFT[m], self.rd(ops[0], ea_var=ea), cnt, sz, self.wr(ops[0], "r", ea)))
            self.L("  }")
        elif m == "imul" and len(ops) in (2, 3) and sz == 4:
            a_ = self.rd(ops[1] if len(ops) == 3 else ops[0])
            b_ = ("0x%Xu" % (ops[2].imm & 0xFFFFFFFF)) if len(ops) == 3 else self.rd(ops[1])
            self.L("  { uint32_t r = aotrt::imul32(c, %s, %s); %s }" % (a_, b_, self.wr(ops[0], "r")))
        elif m == "cdq":
            self.L("  R[2] = (R[0] & 0x80000000u) ? 0xFFFFFFFFu : 0u;")
        elif m == "cwde":
            self.L("  R[0] = cpu::sext(R[0] & 0xFFFFu, 2);")
        elif m.startswith("set") and m[3:] in CC:
            self.L("  " + self.wr(ops[0], "(cpu::cond(c, %d) ? 1u : 0u)" % CC[m[3:]]))
        elif m.startswith("cmov") and m[4:] in CC:
            self.L("  { uint32_t v = %s; if (cpu::cond(c, %d)) { %s } }" % (self.rd(ops[1]), CC[m[4:]], self.wr(ops[0], "v")))
        elif m.startswith("j") and m[1:] in CC:
            t = ops[0].imm & 0xFFFFFFFF
            self.L("  if (cpu::cond(c, %d)) %s" % (CC[m[1:]], self.target_code(t)))
        elif m == "jecxz":
            self.L("  if (R[1] == 0) %s" % self.target_code(ops[0].imm & 0xFFFFFFFF))
        elif m in ("loop", "loope", "loopne"):
            extra = {"loop": "", "loope": " && cpu::get_zf(c)", "loopne": " && !cpu::get_zf(c)"}[m]
            self.L("  if (--R[1] != 0%s) %s" % (extra, self.target_code(ops[0].imm & 0xFFFFFFFF)))
        elif m == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                self.L("  " + self.target_code(op.imm & 0xFFFFFFFF))
            else:
                self.L("  c.eip = %s;" % self.rd(op, 4))
                tbl = self.f.switches.get(a)
                if tbl:
                    self.L("  switch (c.eip) {")
                    for t in sorted(set(tbl)):
                        if t in self.f.insns:
                            self.L("    case 0x%Xu: goto L_%08X;" % (t, t))
                    self.L("    default: return;")
                    self.L("  }")
                else:
                    self.L("  return;")
        elif m == "call":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xFFFFFFFF
                fn = "F_%08X" % t if t in self.rc.translated else None
                self.emit_call(ins, "0x%Xu" % t, fn)
            else:
                self.emit_call(ins, self.rd(op, 4), None)
        elif m == "ret":
            n = ops[0].imm if ops else 0
            self.L("  c.eip = cpu::pop32(c);%s return;" % ((" R[4] += %du;" % n) if n else ""))
        elif m == "leave":
            self.L("  R[4] = R[5]; R[5] = cpu::pop32(c);")
        elif m in ("nop", "fnop", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "prefetch", "prefetchw"):
            pass
        elif m == "xchg" and all(o.type == X.X86_OP_REG for o in ops):
            self.L("  { uint32_t a = %s, b = %s; %s %s }" % (self.rd(ops[0]), self.rd(ops[1]),
                                                             self.wr(ops[0], "b"), self.wr(ops[1], "a")))
        elif m == "cld":
            self.L("  c.eflags &= ~cpu::DF;")
        elif m == "std":
            self.L("  c.eflags |= cpu::DF;")
        elif m == "bswap":
            r = REG32.index(self.rc.md.reg_name(ops[0].reg))
            self.L("  R[%d] = __builtin_bswap32(R[%d]);" % (r, r))
        else:
            if m.startswith("j") or m in ("call", "ret", "retf", "iretd", "int", "int3", "into", "hlt", "ud2", "sysenter", "lcall", "ljmp"):
                self.L("  c.eip = 0x%Xu; return;" % a)  # let the dispatcher/interpreter handle it
                return
            return self.fallback(ins)
        self.native += 1

    def run(self):
        f = self.f
        addrs = sorted(f.insns)
        self.L("void F_%08X(cpu::Cpu& c) {" % f.entry)
        self.L("  uint32_t* R = c.r;")
        if addrs[0] != f.entry:
            self.L("  goto L_%08X;" % f.entry)
        prev_end = None
        prev_falls = False
        for a in addrs:
            ins = f.insns[a]
            if prev_falls and prev_end != a:
                self.L("  %s" % self.target_code(prev_end))
            if a in f.leaders or (prev_end != a):
                self.L("L_%08X:" % a)
            self.emit(ins)
            m = ins.mnemonic
            prev_end = a + ins.size
            prev_falls = not (m in ("ret", "jmp", "hlt", "ud2", "int3", "retf", "iretd"))
        if prev_falls:
            self.L("  c.eip = 0x%Xu; return;" % prev_end)
        self.L("}")
        return self.out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--reloc-rva", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--profile", action="append", default=[])
    ap.add_argument("--out", required=True)
    ap.add_argument("--per-file", type=int, default=400)
    ap.add_argument("--only-profile", action="store_true",
                    help="translate only functions observed on the device (and what they call)")
    a = ap.parse_args()

    img = Image(a.exe, a.reloc_rva)
    rc = Recomp(img)
    profile = set()
    for pth in a.profile:
        if os.path.exists(pth):
            with open(pth) as fh:
                profile |= {int(l, 16) for l in fh if l.strip()}
    seeds = {img.entry} | profile
    if not a.only_profile:
        # Code pointers stored in data (vtables, callback tables) - not in .text, which holds jump tables.
        for s in img.reloc_sites:
            if img.in_text(s):
                continue
            v = img.rd32(s)
            if v is not None and img.in_text(v):
                seeds.add(v)
    rc.discover(seeds)
    funcs = [f for f in rc.funcs.values() if f.ok and f.insns]
    rc.translated = {f.entry for f in funcs}
    print("functions: %d (from %d seeds, %d profiled), instructions: %d" %
          (len(funcs), len(seeds), len(profile), sum(len(f.insns) for f in funcs)))

    os.makedirs(a.out, exist_ok=True)
    for old in os.listdir(a.out):
        if old.startswith("aot_") and old.endswith((".cpp", ".h")):
            os.remove(os.path.join(a.out, old))
    with open(os.path.join(a.out, "aot_decls.h"), "w") as fh:
        fh.write("#pragma once\n// Generated by tools/recomp/recomp.py - do not edit.\n#include \"cpu/aot_rt.h\"\n\n")
        for f in sorted(funcs, key=lambda f: f.entry):
            fh.write("void F_%08X(cpu::Cpu& c);\n" % f.entry)
    funcs.sort(key=lambda f: f.entry)
    native = fallback = 0
    for i in range(0, len(funcs), a.per_file):
        chunk = funcs[i:i + a.per_file]
        n = i // a.per_file
        with open(os.path.join(a.out, "aot_%03d.cpp" % n), "w") as fh:
            fh.write("// Generated by tools/recomp/recomp.py - do not edit.\n#include \"generated/aot_decls.h\"\n\n")
            fh.write("#pragma clang diagnostic ignored \"-Wunused-label\"\n")
            fh.write("#pragma clang diagnostic ignored \"-Wunused-variable\"\n\n")
            for f in chunk:
                e = Emitter(rc, f)
                fh.write("\n".join(e.run()) + "\n\n")
                native += e.native
                fallback += e.fallback
            fh.write("namespace {\nconst aot::Entry kTable[] = {\n")
            for f in chunk:
                fh.write("  {0x%08Xu, F_%08X},\n" % (f.entry, f.entry))
            fh.write("};\nstruct Reg { Reg() { aot::register_table(kTable, %d); } } g_reg;\n}  // namespace\n" % len(chunk))
    print("emitted: %d native, %d interpreter-stepped (%.1f%% native)" %
          (native, fallback, 100.0 * native / max(1, native + fallback)))


if __name__ == "__main__":
    main()
