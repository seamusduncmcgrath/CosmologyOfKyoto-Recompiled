"""Differential testing of the emitter against a reference x86 CPU (Unicorn).

Every distinct integer instruction form found in the program, plus fused compare+branch
pairs, is emitted as C by the real FunctionEmitter, compiled into a DLL and run against
random register/flag/memory states. Unicorn runs the same bytes in 16-bit real mode. Final
registers, flags (excluding flags Intel documents as undefined), memory, branch direction
and divide faults must match.

Memory layout shared by both sides: selector S maps to linear S * 16 (real-mode style), so
the generated code's g_seg_base[S >> 3] = buffer + S * 16 gives identical addressing.
"""
import ctypes
import random
import subprocess
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_16, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from . import flags as F
from .emit import CMP_COND, LOGIC_COND, RESULT_COND, EmitError, FunctionEmitter
from .program import CONDITIONAL_JUMPS, Body, Flow, Insn

SEGS = {"es": 0x2000, "cs": 0x6000, "ss": 0x3000, "ds": 0x1000, "fs": 0x4000, "gs": 0x5000}
CODE_IP = 0xF000
WINDOW = 0x10000
MEM_SIZE = 0x70000
TEST_SEG = 1                                  # SEL(n) for the generated code
# popf is skipped because random stack data would set TF and single-step the reference CPU
SKIP_MNEMS = {"call", "lcall", "ret", "retf", "iret", "int", "lar", "lsl", "verr", "verw", "hlt",
              "ljmp", "nop", "wait", "popf", "popfd"}
FUSABLE = ("cmp", "sub", "test", "and", "or", "xor", "add", "inc", "dec", "neg")


class CPUState(ctypes.Structure):
    _fields_ = [("regs", ctypes.c_uint32 * 8), ("sregs", ctypes.c_uint16 * 6),
                ("cf", ctypes.c_uint8), ("pf", ctypes.c_uint8), ("af", ctypes.c_uint8),
                ("zf", ctypes.c_uint8), ("sf", ctypes.c_uint8), ("of", ctypes.c_uint8),
                ("df", ctypes.c_uint8), ("ifl", ctypes.c_uint8),
                ("fpu", ctypes.c_uint8 * 128)]            # padding; FPU unused by these tests


REG_ORDER = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
SREG_ORDER = ["es", "cs", "ss", "ds", "fs", "gs"]
FLAG_BITS = [("cf", F.CF), ("pf", F.PF), ("af", F.AF), ("zf", F.ZF), ("sf", F.SF), ("of", F.OF)]


@dataclass
class Case:
    name: str
    code: bytes             # instruction bytes (1 or 2 instructions)
    insns: list             # decoded at CODE_IP
    kind: str               # "single" or "fused"
    undefined: int          # flags not compared


def undefined_flags(insn):
    m = insn.mnem
    ops = insn.cs.operands
    if m in ("and", "or", "xor", "test"):
        return F.AF
    if m in ("mul", "imul"):
        return F.SF | F.ZF | F.AF | F.PF
    if m in ("div", "idiv"):
        return F.ALL
    if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
        width = ops[0].size * 8
        count = ops[1].imm & 31 if len(ops) > 1 and ops[1].type == X86_OP_IMM else None
        und = F.AF if m in ("shl", "sal", "shr", "sar") else 0
        if count is None:            # by CL: depends on the random CL; be lenient on OF/CF
            return und | F.OF | (F.CF if m in ("shl", "sal", "shr") else 0)
        if count != 1 and not (insn.cs.opcode[0] in (0xD0, 0xD1)):
            und |= F.OF
        if m in ("shl", "sal", "shr") and count >= width:
            und |= F.CF
        return und
    return 0


def collect_cases(progs, per_form=4):
    md = Cs(CS_ARCH_X86, CS_MODE_16)
    md.detail = True
    forms = defaultdict(list)
    fused = defaultdict(list)
    for prog in progs:
        for entry in prog.functions:
            body = prog.discover(*entry)
            for a, insn in body.insns.items():
                if insn.fixups or insn.mnem in SKIP_MNEMS or insn.cs.opcode[0] in range(0xD8, 0xE0):
                    continue
                fl = body.flows[a]
                if fl.kind not in ("next", "jcc", "jmp"):
                    continue
                sig = (insn.mnem, tuple((o.type, o.size) for o in insn.cs.operands),
                       tuple(p for p in insn.cs.prefix if p))
                if len(forms[sig]) < per_form and insn.cs.bytes not in [c[0] for c in forms[sig]]:
                    forms[sig].append((bytes(insn.cs.bytes), insn.mnem))
                nxt = body.insns.get(insn.end)
                table = CMP_COND if insn.mnem in ("cmp", "sub") else \
                    LOGIC_COND if insn.mnem in ("test", "and", "or", "xor") else RESULT_COND
                if insn.mnem in FUSABLE and nxt is not None and nxt.mnem in table:
                    key = (insn.mnem, nxt.mnem, insn.cs.operands[0].size)
                    if len(fused[key]) < 2:
                        fused[key].append(bytes(insn.cs.bytes) + bytes(nxt.cs.bytes))
    cases = []
    for sig, examples in sorted(forms.items(), key=str):
        for i, (code, mnem) in enumerate(examples):
            insns = _decode_all(md, code)
            if insns:
                cases.append(Case(f"{mnem}_{len(cases)}", code, insns, "single", undefined_flags(insns[0])))
    for (m, j, size), examples in sorted(fused.items(), key=str):
        for code in examples:
            insns = _decode_all(md, code)
            if insns and len(insns) == 2:
                cases.append(Case(f"{m}_{j}_{len(cases)}", code, insns, "fused", 0))
    return cases


def _decode_all(md, code):
    out, off = [], CODE_IP
    for ci in md.disasm(code, CODE_IP):
        out.append(Insn(TEST_SEG, ci.address, ci.size, ci.mnemonic, ci.op_str, ci, []))
        off += ci.size
    return out if off == CODE_IP + len(code) else None


class _FakeProg:
    functions = {}


def emit_case(case, idx):
    """C function `int t_<idx>(CPU *c)`: 0 = fell through, 1 = branch taken."""
    body = Body((TEST_SEG, CODE_IP), case.name)
    targets = set()
    for insn in case.insns:
        body.insns[insn.off] = insn
        if insn.mnem in CONDITIONAL_JUMPS or insn.mnem == "jmp":
            t = insn.cs.operands[0].imm & 0xFFFF
            body.flows[insn.off] = Flow("jcc" if insn.mnem != "jmp" else "jmp", (t,), insn.mnem != "jmp")
            targets.add(t)
        else:
            body.flows[insn.off] = Flow("next")
    body.labels = set(targets)
    if case.kind == "fused":
        j = case.insns[1]
        live = {case.insns[0].off: F.JCC_USES[j.mnem], j.off: 0}
    else:
        live = defaultdict(lambda: F.ALL)
    em = FunctionEmitter("t", _FakeProg(), body, live)
    em.imports = lambda imp: 0
    first = case.insns[0]
    if case.kind == "fused":
        code = em.try_fuse(first, case.insns[1].off)
        if code is None:
            raise EmitError("fusion not applicable")
    else:
        code = em.control(first, body.flows[first.off]) if first.off in body.flows and \
            body.flows[first.off].kind in ("jcc", "jmp") else em.insn(first)
    lines = [f"/* {case.name}: {' ; '.join(str(i) for i in case.insns)} */",
             f"int t_{idx}(CPU *c)", "{"]
    lines += [f"    {l}" for l in code]
    lines.append("    return 0;")
    for t in sorted(targets):
        lines.append(f"L_{t:04x}: return 1;")
    lines.append("}")
    return "\n".join(lines)


HARNESS = r"""
#include "recomp.h"
#define SEL(n) ((uint16_t)0x6000)
#define IMPCALL(i) ((void)0)
void rt_import(CPU *c, const RecompModule *m, uint32_t i) { (void)c; (void)m; (void)i; }
#define IMPADDR(i) 0u
uint8_t *g_seg_base[8192];
static jmp_buf fault;
void rt_divide_error(CPU *c, uint16_t s, uint16_t o) { (void)c; (void)s; (void)o; longjmp(fault, 1); }
void rt_trap(CPU *c, const char *w, uint16_t s, uint16_t o) { (void)c; (void)w; (void)s; (void)o; longjmp(fault, 2); }
void rt_call_near(CPU *c, uint16_t s, uint16_t o) { (void)c; (void)s; (void)o; longjmp(fault, 2); }
void rt_call_far(CPU *c, uint16_t s, uint16_t o) { (void)c; (void)s; (void)o; longjmp(fault, 2); }
%TESTS%
typedef int (*TestFn)(CPU *);
static const TestFn tests[] = { %TABLE% };
__declspec(dllexport) void set_memory(uint8_t *buf)
{
    static const uint16_t sels[] = { 0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000 };
    for (int i = 0; i < 6; i++) g_seg_base[sels[i] >> 3] = buf + (uint32_t)sels[i] * 16;
}
/* returns 0/1 = branch not taken/taken, 2 = divide fault, 3 = other trap */
__declspec(dllexport) int run_test(int idx, CPU *c)
{
    int r = setjmp(fault);
    if (r) return r == 1 ? 2 : 3;
    return tests[idx](c);
}
"""


def build_dll(cases, outdir, include_dir):
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    tests, table, usable = [], [], []
    for case in cases:
        try:
            tests.append(emit_case(case, len(usable)))
        except EmitError as e:
            print(f"skip {case.name}: {e}")
            continue
        table.append(f"t_{len(usable)}")
        usable.append(case)
    src = outdir / "insntest.c"
    src.write_text(HARNESS.replace("%TESTS%", "\n\n".join(tests)).replace("%TABLE%", ", ".join(table)))
    dll = outdir / "insntest.dll"
    cmd = ["clang", "-std=c11", "-O1", "-w", "-shared", "-fuse-ld=lld", f"-I{include_dir}", str(src), "-o", str(dll)]
    subprocess.run(cmd, check=True)
    return dll, usable


def run(cases, dll_path, vectors=64, seed=1, verbose=False):
    from unicorn import UC_ARCH_X86, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_16, Uc, UcError
    from unicorn import x86_const as X

    lib = ctypes.CDLL(str(dll_path))
    lib.run_test.argtypes = [ctypes.c_int, ctypes.POINTER(CPUState)]
    buf = (ctypes.c_uint8 * MEM_SIZE)()
    lib.set_memory(buf)
    faulted = []
    writes = []

    def make_uc(image=None):
        # Unicorn doesn't reset its exception state between runs: after one fault the next
        # run reports a double fault and later ones nothing. Use a fresh engine after faults.
        u = Uc(UC_ARCH_X86, UC_MODE_16)
        u.mem_map(0, 0x100000)
        u.hook_add(UC_HOOK_INTR, lambda uc_, intno, _: (faulted.append(intno), uc_.emu_stop()))
        u.hook_add(UC_HOOK_MEM_WRITE, lambda uc_, access, addr, size, value, _: writes.append((addr, size)))
        if image is not None:
            u.mem_write(0, image)
        return u

    uc = make_uc()
    ureg = {n: getattr(X, f"UC_X86_REG_{n.upper()}") for n in REG_ORDER + SREG_ORDER}
    rng = random.Random(seed)
    failures = defaultdict(list)

    for idx, case in enumerate(cases):
        image = bytearray(rng.randbytes(MEM_SIZE))
        image[SEGS["cs"] * 16 + CODE_IP:SEGS["cs"] * 16 + CODE_IP + len(case.code)] = case.code
        image = bytes(image)
        # memory is written once per case; after each run only changed bytes are restored
        uc.mem_write(0, image)
        ctypes.memmove(buf, image, MEM_SIZE)
        for v in range(vectors):
            regs = [rng.getrandbits(32) for _ in range(8)]
            for r in (3, 4, 5, 6, 7):        # bx, sp, bp, si, di: keep addresses away from 64K edges
                regs[r] = (regs[r] & 0xFFFF0000) | rng.randrange(0x0100, 0xF000)
            if rng.random() < 0.3:
                regs[1] = (regs[1] & 0xFFFF0000) | rng.randrange(0, 4)   # small CX/CL often
            if 0x67 in case.code[:4]:        # 32-bit addressing: offsets must stay inside 64K
                for r in (3, 4, 5, 6, 7):
                    regs[r] &= 0xFFFF
                regs[1] &= 0xFFFF
            flags = {n: rng.getrandbits(1) for n, _ in FLAG_BITS}
            flags["df"] = rng.getrandbits(1) if rng.random() < 0.2 else 0
            # C side
            st = CPUState()
            for i, r in enumerate(regs):
                st.regs[i] = r
            for i, s in enumerate(SREG_ORDER):
                st.sregs[i] = SEGS[s]
            for n, val in flags.items():
                setattr(st, n, val)
            st.ifl = 1
            c_result = lib.run_test(idx, ctypes.byref(st))
            c_mem = ctypes.string_at(buf, MEM_SIZE)
            # Unicorn side
            writes.clear()
            for n, r in zip(REG_ORDER, regs):
                uc.reg_write(ureg[n], r)
            for s in SREG_ORDER:
                uc.reg_write(ureg[s], SEGS[s])
            eflags = 0x202 | sum(bit for n, bit in FLAG_BITS if flags[n]) | (0x400 if flags["df"] else 0)
            uc.reg_write(X.UC_X86_REG_EFLAGS, eflags)
            faulted.clear()
            start = SEGS["cs"] * 16 + CODE_IP
            end = start + len(case.code)
            try:
                # no timeout: Unicorn arms a watchdog thread per call (~15 ms on Windows),
                # and nothing tested can run unbounded (rep stops when CX reaches 0)
                uc.emu_start(start, end, count=0 if case.kind == "single" and
                             case.insns[0].mnem.startswith("rep") else len(case.insns))
            except UcError as e:
                faulted.append(str(e))
            ip = uc.reg_read(X.UC_X86_REG_IP)
            branches = any(i.mnem in CONDITIONAL_JUMPS or i.mnem == "jmp" for i in case.insns)
            if ip == CODE_IP and not branches:
                faulted.append("no progress")        # the instruction raised an exception
            u_result = 2 if faulted else (0 if ip == CODE_IP + len(case.code) else 1)
            problems = []
            if (c_result == 2) != bool(faulted):
                problems.append(f"fault: C={c_result} unicorn={faulted}")
            elif not faulted:
                if c_result != u_result and any(i.mnem in CONDITIONAL_JUMPS for i in case.insns):
                    problems.append(f"branch: C={'taken' if c_result else 'not'} unicorn={'taken' if u_result else 'not'}")
                for i, n in enumerate(REG_ORDER):
                    u = uc.reg_read(ureg[n])
                    if st.regs[i] != u:
                        problems.append(f"{n}: C={st.regs[i]:#010x} unicorn={u:#010x}")
                for i, s in enumerate(SREG_ORDER):
                    u = uc.reg_read(ureg[s])
                    if s != "cs" and st.sregs[i] != u:
                        problems.append(f"{s}: C={st.sregs[i]:#06x} unicorn={u:#06x}")
                if case.kind == "single":
                    uf = uc.reg_read(X.UC_X86_REG_EFLAGS)
                    for n, bit in FLAG_BITS:
                        if not case.undefined & bit and getattr(st, n) != bool(uf & bit):
                            problems.append(f"{n}: C={getattr(st, n)} unicorn={int(bool(uf & bit))}")
                    if st.df != bool(uf & 0x400):
                        problems.append(f"df: C={st.df} unicorn={int(bool(uf & 0x400))}")
                if writes or c_mem != image:
                    u_mem = bytearray(image)
                    for addr, size in writes:
                        if addr + size <= MEM_SIZE:
                            u_mem[addr:addr + size] = uc.mem_read(addr, size)
                    if bytes(u_mem) != c_mem:
                        diff = next(k for k in range(MEM_SIZE) if u_mem[k] != c_mem[k])
                        problems.append(f"memory differs first at {diff:#x}: C={c_mem[diff]:#04x} unicorn={u_mem[diff]:#04x}")
            if problems:
                failures[case.name].append((regs, flags, problems))
            if faulted:
                uc = make_uc(image)
            else:
                for addr, size in writes:
                    if addr + size <= MEM_SIZE:
                        uc.mem_write(addr, image[addr:addr + size])
            if c_mem != image:
                ctypes.memmove(buf, image, MEM_SIZE)
        if verbose and case.name in failures:
            print(f"FAIL {case.name}: {' ; '.join(str(i) for i in case.insns)} -> {failures[case.name][0][2][:3]}")
    return failures
