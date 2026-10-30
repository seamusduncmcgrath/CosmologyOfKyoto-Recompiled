"""Differential testing of x87 emission + runtime/src/fpu.c against Unicorn.

Each FPU instruction form found in the program is wrapped in a fixed sequence:

    fninit ; fld qword [A] ; fld qword [B] ; <op> ; fnstsw [SW] ; fstp qword [R0] ; fstp qword [R1]

with any memory operand of <op> redirected to [C], which holds a third value in the
operand's format. Results are compared as doubles with a small tolerance (the runtime keeps
doubles, real x87 computes in 80 bits), along with condition codes C0-C3 and TOP. Extra
hand-written sequences cover rounding control (fldcw + frndint/fistp).
"""
import ctypes
import math
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_16, Cs

from .emit import EmitError, FunctionEmitter
from .insntest import CODE_IP, SEGS, TEST_SEG, CPUState, _FakeProg
from .program import Body, Flow, Insn

A, B, C, R0, R1, SW, AXOUT = 0x100, 0x108, 0x110, 0x120, 0x128, 0x130, 0x132
DS_LIN = SEGS["ds"] * 16

# stack depth change of each op (after the two loads, depth is 2)
DEPTH = {"fld": 1, "fild": 1, "fld1": 1, "fldz": 1, "fldpi": 1, "fldlg2": 1, "fldln2": 1,
         "fstp": -1, "fistp": -1, "faddp": -1, "fsubp": -1, "fsubrp": -1, "fmulp": -1, "fdivp": -1,
         "fdivrp": -1, "fcomp": -1, "fcompp": -2, "fptan": 1, "fpatan": -1, "fyl2x": -1}
SKIP = {"fnstenv", "fninit", "fnclex", "fldcw", "fnstcw", "wait"}   # covered by sequences or trivial
# operand values by instruction (A is ST(1), B is ST(0), Cval is the memory operand)
VALUES = [(1.5, -2.25, 3.0), (100.125, 0.1, -7.0), (-0.5, 1e10, 0.25), (3.0, 3.0, 3.0), (2.0, 0.0, 1.0)]
DOMAIN = {
    "fsqrt": [(1.0, 2.0, 0), (5.0, 1e-3, 0), (0, 144.0, 0)],
    "fsin": [(0, 0.5, 0), (0, -1.25, 0), (0, 3.0, 0)],
    "fcos": [(0, 0.5, 0), (0, -1.25, 0), (0, 3.0, 0)],
    "fptan": [(0, 0.5, 0), (0, -0.7, 0), (0, 0.1, 0)],
    "fpatan": [(1.0, 2.0, 0), (-3.0, 0.5, 0), (0.25, -4.0, 0)],
    "f2xm1": [(0, 0.5, 0), (0, -0.75, 0), (0, 0.125, 0)],
    "fyl2x": [(2.0, 8.0, 0), (-1.5, 0.3, 0), (3.0, 1e5, 0)],
    "fscale": [(3.0, 1.5, 0), (-2.0, 10.0, 0), (5.7, -1.0, 0)],
    "frndint": [(0, 2.5, 0), (0, -3.5, 0), (0, 1.25, 0), (0, -0.75, 0)],
    "fprem": [(3.0, 10.5, 0), (2.5, -7.25, 0), (0.75, 100.0, 0)],
}
TRANSCENDENTAL = {"fsin", "fcos", "fptan", "fpatan", "f2xm1", "fyl2x"}


def f80(v):
    if v == 0:
        return b"\0" * 8 + (b"\x00\x80" if math.copysign(1, v) < 0 else b"\0\0")
    m, e = math.frexp(abs(v))
    mant = int(m * (1 << 64))
    se = (e - 1 + 16383) | (0x8000 if v < 0 else 0)
    return struct.pack("<QH", mant, se)


def encode_operand(size, kind, v):
    if kind == "int":
        return struct.pack({2: "<h", 4: "<i", 8: "<q"}[size], int(v))
    if size == 4:
        return struct.pack("<f", v)
    if size == 8:
        return struct.pack("<d", v)
    return f80(v)


@dataclass
class FpuCase:
    name: str
    op_bytes: bytes
    mnem: str
    mem: tuple | None           # (size, "float"/"int", is_store)
    values: list
    depth: int
    prologue: bytes = b""       # e.g. fldcw [C+...]


def collect_fpu_cases(progs):
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    md.detail = True
    seen = {}
    for prog in progs:
        for entry in prog.functions:
            body = prog.discover(*entry)
            for insn in body.insns.values():
                op = insn.cs.opcode[0]
                if op not in range(0xD8, 0xE0) or insn.mnem in SKIP:
                    continue
                modrm = insn.cs.modrm
                key = (op, modrm >> 3 & 7, None if modrm >> 6 != 3 else modrm & 7)
                if key in seen:
                    continue
                if modrm >> 6 == 3:
                    code = bytes([op, modrm])
                    mem = None
                else:
                    code = bytes([op, (modrm & 0x38) | 6]) + struct.pack("<H", C)
                    memop = [o for o in insn.cs.operands if o.type == 3][0]
                    size = memop.size
                    is_int = op in (0xDA, 0xDE) or (op in (0xDB, 0xDF) and (modrm >> 3 & 7) in (0, 2, 3, 5, 7)
                                                      and not (op == 0xDB and (modrm >> 3 & 7) in (5, 7)))
                    is_store = insn.mnem.startswith(("fst", "fist")) or insn.mnem == "fnstsw"
                    mem = (size, "int" if is_int else "float", is_store)
                seen[key] = FpuCase(f"{insn.mnem}_{len(seen)}", code, insn.mnem, mem,
                                    DOMAIN.get(insn.mnem, VALUES), DEPTH.get(insn.mnem, 0))
    cases = list(seen.values())
    # rounding control: fldcw [C+8] then frndint / fistp qword [C]
    for rc, cw in ((0, 0x037F), (1, 0x077F), (2, 0x0B7F), (3, 0x0F7F)):
        fldcw = bytes([0xD9, 0x2E]) + struct.pack("<H", C + 8)
        cases.append(FpuCase(f"frndint_rc{rc}", bytes([0xD9, 0xFC]), "frndint", None,
                             [(0, v, cw) for v in (2.5, -2.5, 3.5, 1.25, -0.75)], 0, fldcw))
        cases.append(FpuCase(f"fistp_rc{rc}", bytes([0xDF, 0x3E]) + struct.pack("<H", C), "fistp",
                             (8, "int", True), [(0, v, cw) for v in (2.5, -2.5, 3.5, 1.25, -0.75)], -1, fldcw))
    return cases


def sequence(case):
    return (bytes([0xDB, 0xE3])                                   # fninit
            + bytes([0xDD, 0x06]) + struct.pack("<H", A)           # fld qword [A]
            + bytes([0xDD, 0x06]) + struct.pack("<H", B)           # fld qword [B]
            + case.prologue + case.op_bytes
            + bytes([0xDD, 0x3E]) + struct.pack("<H", SW)          # fnstsw [SW]
            + bytes([0xDD, 0x1E]) + struct.pack("<H", R0)          # fstp qword [R0]
            + bytes([0xDD, 0x1E]) + struct.pack("<H", R1))         # fstp qword [R1]


def emit_fpu_case(case, idx):
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    md.detail = True
    code = sequence(case)
    body = Body((TEST_SEG, CODE_IP), case.name)
    lines = [f"/* {case.name} */", f"int t_{idx}(CPU *c)", "{"]
    em = FunctionEmitter("t", _FakeProg(), body, {})
    for ci in md.disasm(code, CODE_IP):
        insn = Insn(TEST_SEG, ci.address, ci.size, ci.mnemonic, ci.op_str, ci, [])
        body.insns[insn.off] = insn
        body.flows[insn.off] = Flow("next")
        em.live = {insn.off: 0}
        lines += [f"    {l}" for l in em.insn(insn)]
    lines += ["    return 0;", "}"]
    return "\n".join(lines)


HARNESS = r"""
#include "recomp.h"
#define SEL(n) ((uint16_t)0x6000)
uint8_t *g_seg_base[8192];
void rt_import(CPU *c, const RecompModule *m, uint32_t i) { (void)c; (void)m; (void)i; }
%TESTS%
typedef int (*TestFn)(CPU *);
static const TestFn tests[] = { %TABLE% };
__declspec(dllexport) void set_memory(uint8_t *buf)
{
    static const uint16_t sels[] = { 0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000 };
    for (int i = 0; i < 6; i++) g_seg_base[sels[i] >> 3] = buf + (uint32_t)sels[i] * 16;
}
__declspec(dllexport) int run_test(int idx, CPU *c) { return tests[idx](c); }
"""


def build_and_run(cases, outdir="build/fputest"):
    from unicorn import UC_ARCH_X86, UC_MODE_16, Uc
    from unicorn import x86_const as X

    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    tests, table, usable = [], [], []
    for case in cases:
        try:
            tests.append(emit_fpu_case(case, len(usable)))
        except EmitError as e:
            print(f"skip {case.name}: {e}")
            continue
        table.append(f"t_{len(usable)}")
        usable.append(case)
    src = out / "fputest.c"
    src.write_text(HARNESS.replace("%TESTS%", "\n\n".join(tests)).replace("%TABLE%", ", ".join(table)))
    dll = out / "fputest.dll"
    subprocess.run(["clang", "-std=c11", "-O1", "-w", "-shared", "-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS",
                    "-Iruntime/include", "-Iruntime/src", str(src), "runtime/src/fpu.c", "runtime/src/log.c",
                    "-o", str(dll)], check=True)

    lib = ctypes.CDLL(str(dll))
    lib.run_test.argtypes = [ctypes.c_int, ctypes.POINTER(CPUState)]
    mem_size = 0x70000
    buf = (ctypes.c_uint8 * mem_size)()
    lib.set_memory(buf)
    failures = {}
    for idx, case in enumerate(usable):
        code = sequence(case)
        tol = 1e-13 if case.mnem in TRANSCENDENTAL else 1e-15
        for a, b, cval in case.values:
            image = bytearray(mem_size)
            image[SEGS["cs"] * 16 + CODE_IP:SEGS["cs"] * 16 + CODE_IP + len(code)] = code
            image[DS_LIN + A:DS_LIN + A + 8] = struct.pack("<d", a)
            image[DS_LIN + B:DS_LIN + B + 8] = struct.pack("<d", b)
            if case.prologue:
                image[DS_LIN + C + 8:DS_LIN + C + 10] = struct.pack("<H", cval)
            elif case.mem and not case.mem[2]:
                enc = encode_operand(case.mem[0], case.mem[1], cval)
                image[DS_LIN + C:DS_LIN + C + len(enc)] = enc
            image = bytes(image)
            # C side
            ctypes.memmove(buf, image, mem_size)
            st = CPUState()
            for i, s in enumerate(("es", "cs", "ss", "ds", "fs", "gs")):
                st.sregs[i] = SEGS[s]
            lib.run_test(idx, ctypes.byref(st))
            c_mem = ctypes.string_at(buf, mem_size)
            c_ax = st.regs[0] & 0xFFFF
            # Unicorn side
            uc = Uc(UC_ARCH_X86, UC_MODE_16)
            uc.mem_map(0, 0x100000)
            uc.mem_write(0, image)
            for s, v in SEGS.items():
                uc.reg_write(getattr(X, f"UC_X86_REG_{s.upper()}"), v)
            start = SEGS["cs"] * 16 + CODE_IP
            uc.emu_start(start, start + len(code))
            u_mem = bytes(uc.mem_read(0, mem_size))
            u_ax = uc.reg_read(X.UC_X86_REG_AX)
            problems = []
            depth = 2 + case.depth
            for k, loc in enumerate((R0, R1)):
                if k >= depth:
                    break
                cv = struct.unpack("<d", c_mem[DS_LIN + loc:DS_LIN + loc + 8])[0]
                uv = struct.unpack("<d", u_mem[DS_LIN + loc:DS_LIN + loc + 8])[0]
                if not close(cv, uv, tol):
                    problems.append(f"ST({k}): C={cv!r} unicorn={uv!r}")
            csw = struct.unpack("<H", c_mem[DS_LIN + SW:DS_LIN + SW + 2])[0]
            usw = struct.unpack("<H", u_mem[DS_LIN + SW:DS_LIN + SW + 2])[0]
            mask = 0x4700 | 0x3800 if case.mnem != "fxam" else 0x4700 | 0x3800
            if case.mnem in ("fxam", "fprem", "fprem1"):
                mask |= 0x0200
            if (csw & mask) != (usw & mask):
                problems.append(f"status: C={csw:#06x} unicorn={usw:#06x}")
            if case.mnem == "fnstsw" and not case.mem and c_ax != u_ax:
                problems.append(f"ax: C={c_ax:#06x} unicorn={u_ax:#06x}")
            if case.mem and case.mem[2]:
                size, kind, _ = case.mem
                cb, ub = c_mem[DS_LIN + C:DS_LIN + C + size], u_mem[DS_LIN + C:DS_LIN + C + size]
                if kind == "float" and size in (4, 8):
                    fmt = "<f" if size == 4 else "<d"
                    if not close(struct.unpack(fmt, cb)[0], struct.unpack(fmt, ub)[0], 1e-6 if size == 4 else tol):
                        problems.append(f"store: C={cb.hex()} unicorn={ub.hex()}")
                elif kind == "float" and size == 10:
                    if not close(f80_to_float(cb), f80_to_float(ub), tol):
                        problems.append(f"store80: C={cb.hex()} unicorn={ub.hex()}")
                elif cb != ub:
                    problems.append(f"store: C={cb.hex()} unicorn={ub.hex()}")
            if problems:
                failures.setdefault(case.name, []).append(((a, b, cval), problems))
    return usable, failures


def f80_to_float(b):
    mant, se = struct.unpack("<QH", b)
    exp = se & 0x7FFF
    if exp == 0 and mant == 0:
        return 0.0
    v = math.ldexp(mant, exp - 16383 - 63)
    return -v if se & 0x8000 else v


def close(x, y, tol):
    if math.isnan(x) and math.isnan(y):
        return True
    if x == y:
        return True
    return abs(x - y) <= tol * max(abs(x), abs(y))
