"""C code generation for one function body (see runtime/include/recomp.h for the contract)."""
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from . import flags as F
from .ne import FARPTR, OFFSET, SELECTOR, Import, Internal


class EmitError(Exception):
    pass


CT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t"}
MASK = {1: "0xFF", 2: "0xFFFF", 4: "0xFFFFFFFFu"}
SIGN = {1: "0x80", 2: "0x8000", 4: "0x80000000u"}
BITS = {1: 8, 2: 16, 4: 32}

REG32 = {"eax": "a", "ecx": "c", "edx": "d", "ebx": "b", "esp": "sp", "ebp": "bp", "esi": "si", "edi": "di"}
REG16 = {"ax": "a", "cx": "c", "dx": "d", "bx": "b", "sp": "sp", "bp": "bp", "si": "si", "di": "di"}
REG8 = {"al": ("a", "l"), "cl": ("c", "l"), "dl": ("d", "l"), "bl": ("b", "l"),
        "ah": ("a", "h"), "ch": ("c", "h"), "dh": ("d", "h"), "bh": ("b", "h")}
SREGS = {"es", "cs", "ss", "ds", "fs", "gs"}
SEG_PREFIX = {0x26: "es", 0x2E: "cs", 0x36: "ss", 0x3E: "ds", 0x64: "fs", 0x65: "gs"}

# Kernel absolute "offset" exports used as constants by the C runtime's huge-pointer code.
IMPORT_CONSTANTS = {("KERNEL", 113): 3, ("KERNEL", 114): 8}   # __AHSHIFT, __AHINCR

# setjmp/longjmp-like routines (see rt_jmp_slot in recomp.h). Each takes a far pointer to
# its buffer as the first stacked argument.
SETJMP_FUNCS = {"_setjmp"}
SETJMP_IMPORTS = {("KERNEL", 55)}                              # Catch
LONGJMP_FUNCS = {"_longjmp"}                                   # (KERNEL Throw: runtime)

JCC_COND = {
    "jo": "c->of", "jno": "!c->of", "jb": "c->cf", "jae": "!c->cf", "je": "c->zf",
    "jne": "!c->zf", "jbe": "(c->cf || c->zf)", "ja": "(!c->cf && !c->zf)", "js": "c->sf",
    "jns": "!c->sf", "jp": "c->pf", "jnp": "!c->pf", "jl": "(c->sf != c->of)",
    "jge": "c->sf == c->of", "jle": "(c->zf || c->sf != c->of)", "jg": "(!c->zf && c->sf == c->of)",
}
# Fused compare-and-branch: condition on (a, b) of a CMP/SUB, and on r for logic ops (OF=CF=0).
CMP_COND = {
    "je": "a == b", "jne": "a != b", "jb": "a < b", "jae": "a >= b", "jbe": "a <= b", "ja": "a > b",
    "jl": "(S)a < (S)b", "jge": "(S)a >= (S)b", "jle": "(S)a <= (S)b", "jg": "(S)a > (S)b",
    "js": "(S)(T)(a - b) < 0", "jns": "(S)(T)(a - b) >= 0",
}
RESULT_COND = {"je": "r == 0", "jne": "r != 0", "js": "(S)r < 0", "jns": "(S)r >= 0"}
LOGIC_COND = dict(RESULT_COND, jl="(S)r < 0", jge="(S)r >= 0", jle="(S)r <= 0", jg="(S)r > 0")


def flag_code(kind, need, size, cin=False):
    """C statements setting the flags in `need` after an operation on a, b -> r."""
    s, out = SIGN[size], []
    if not need:
        return out
    if need & F.CF:
        cf = {
            "add": "(cin ? r <= a : r < a)" if cin else "r < a",
            "sub": "(cin ? a <= b : a < b)" if cin else "a < b",
            "logic": "0", "neg": "a != 0",
        }.get(kind)
        if cf is not None:
            out.append(f"c->cf = {cf};")
    if need & F.OF:
        of = {
            "add": f"((a ^ r) & (b ^ r) & {s}) != 0",
            "sub": f"((a ^ b) & (a ^ r) & {s}) != 0",
            "logic": "0", "inc": f"r == {s}", "dec": f"a == {s}", "neg": f"a == {s}",
        }[kind]
        out.append(f"c->of = {of};")
    if need & F.AF:
        af = {"add": "((a ^ b ^ r) & 0x10) != 0", "sub": "((a ^ b ^ r) & 0x10) != 0", "logic": "0",
              "inc": "(r & 0xF) == 0", "dec": "(r & 0xF) == 0xF", "neg": "((a ^ r) & 0x10) != 0"}[kind]
        out.append(f"c->af = {af};")
    if need & F.ZF:
        out.append("c->zf = r == 0;")
    if need & F.SF:
        out.append(f"c->sf = (r & {s}) != 0;")
    if need & F.PF:
        out.append("c->pf = parity8(r);")
    return out


class FunctionEmitter:
    def __init__(self, mod, prog, body, live_out):
        self.mod = mod                  # C name prefix of the module
        self.prog = prog
        self.body = body
        self.live = live_out            # {offset: live-out flag mask}
        self.seg = body.entry[0]
        self.need_labels = set(body.labels)
        self.imports = None             # set by ModuleEmitter: callable Import -> index

    # -- naming -------------------------------------------------------------------------
    def fname(self, seg, off):
        return f"{self.mod}_{seg:03d}_{off:04x}"

    def sel(self, seg):
        return f"SEL({seg})"

    # -- operands -------------------------------------------------------------------------
    def reg(self, insn, regid):
        name = insn.cs.reg_name(regid)
        if name in REG32:
            return f"c->{REG32[name]}.e", 4
        if name in REG16:
            return f"c->{REG16[name]}.x", 2
        if name in REG8:
            r, half = REG8[name]
            return f"c->{r}.b.{half}", 1
        if name in SREGS:
            return (self.sel(self.seg) if name == "cs" else f"c->{name}"), 2
        raise EmitError(f"unsupported register {name}")

    def seg_override(self, insn):
        return SEG_PREFIX.get(insn.cs.prefix[1])

    def address(self, insn, op):
        """(segment expr, offset expr) of a memory operand."""
        m = op.mem
        if any(f.site >= insn.off + insn.cs.disp_offset and f.site < insn.off + insn.cs.disp_offset + insn.cs.disp_size
               for f in insn.fixups if insn.cs.disp_size):
            raise EmitError("relocated displacement")
        base = insn.cs.reg_name(m.base) if m.base else None
        index = insn.cs.reg_name(m.index) if m.index else None
        wide = insn.cs.addr_size == 4
        parts = []
        for r in (base, index):
            if r is None:
                continue
            reg = (f"c->{REG32[r]}.e" if wide else f"c->{REG16[r]}.x")
            parts.append(reg if r != index or m.scale == 1 else f"{reg} * {m.scale}")
        if m.disp or not parts:
            parts.append(f"{m.disp & (0xFFFFFFFF if wide else 0xFFFF):#x}")
        off = " + ".join(parts)
        off = f"(uint32_t)({off})" if wide else f"(uint16_t)({off})" if len(parts) > 1 else parts[0]
        if m.segment:
            segname = insn.cs.reg_name(m.segment)
        elif base in ("bp", "ebp", "esp"):
            segname = "ss"
        else:
            segname = "ds"
        segexpr = self.sel(self.seg) if segname == "cs" else f"c->{segname}"
        return segexpr, off

    def imm(self, insn, value, size):
        """An immediate, honouring relocations on its bytes."""
        start = insn.off + insn.cs.imm_offset
        fix = [f for f in insn.fixups if start <= f.site < start + max(insn.cs.imm_size, 1)]
        if not fix:
            return f"{value & int(MASK[size].rstrip('u'), 16):#x}"
        f = fix[0]
        if f.kind == SELECTOR and isinstance(f.target, Internal):
            return self.sel(f.target.seg)
        if f.kind == OFFSET and isinstance(f.target, Import):
            key = (f.target.module, f.target.ordinal)
            if key in IMPORT_CONSTANTS:
                return str(IMPORT_CONSTANTS[key])
        if isinstance(f.target, Import) and f.kind in (SELECTOR, FARPTR, OFFSET):
            # parts of an imported function's address, e.g. stored to call through later
            idx = self.imports(f.target)
            addr = f"IMPADDR({idx})"
            return {FARPTR: addr, SELECTOR: f"(uint16_t)({addr} >> 16)", OFFSET: f"(uint16_t){addr}"}[f.kind]
        raise EmitError(f"unsupported relocated immediate {f}")

    def read(self, insn, op):
        if op.type == X86_OP_REG:
            return self.reg(insn, op.reg)[0]
        if op.type == X86_OP_MEM:
            seg, off = self.address(insn, op)
            return f"rd{BITS[op.size]}({seg}, {off})"
        if op.type == X86_OP_IMM:
            return self.imm(insn, op.imm, op.size)
        raise EmitError("bad operand")

    def write(self, insn, op, val):
        if op.type == X86_OP_REG:
            name = insn.cs.reg_name(op.reg)
            if name == "cs":
                raise EmitError("write to CS")
            return f"{self.reg(insn, op.reg)[0]} = {val};"
        if op.type == X86_OP_MEM:
            seg, off = self.address(insn, op)
            return f"wr{BITS[op.size]}({seg}, {off}, {val});"
        raise EmitError("bad destination")

    # -- emission ---------------------------------------------------------------------------
    def emit(self):
        body = self.body
        order = sorted(body.insns)
        lines = []
        self.longjmp_like = self.prog.functions.get(body.entry) in LONGJMP_FUNCS
        if self.longjmp_like:
            # the buffer argument, read before the body changes SP
            lines.append("    uint32_t jmp_key = rd32(c->ss, (uint16_t)(c->sp.x + 4));")
        self.has_ijmp = any(fl.kind == "ijmp" for fl in body.flows.values())
        if self.has_ijmp:
            # An indirect jump to our own return address is a return (e.g. the C runtime's
            # __setargv pops it, lowers SP for argv, then `jmp dword ptr [saved]`), so note
            # it on entry. Whether we were called near or far, compare the matching part.
            lines.append("    uint16_t ret_ip = rd16(c->ss, c->sp.x), ret_cs = rd16(c->ss, (uint16_t)(c->sp.x + 2));")
            lines.append("    (void)ret_ip; (void)ret_cs;")
        entry = body.entry[1]
        if order and order[0] != entry:
            # code is emitted in address order, and bodies can include code below the entry
            # (e.g. an error path jumped back to), so start at the entry explicitly
            lines.append(f"    goto L_{entry:04x};")
            self.need_labels.add(entry)
        skip = set()
        trap_labels = set()
        patched = self.prog.ne.patched_prologs().get(body.entry)
        if patched:
            # what the Windows loader leaves at an exported DLL entry (see patched_prologs)
            after = entry + patched
            if after not in body.insns:
                raise EmitError("patched prologue not followed by code")
            lines.append(f"L_{entry:04x}:")
            lines.append(f"    c->a.x = {self.sel(self.prog.ne.autodata)};   /* loader-patched: mov ax, DGROUP */")
            lines.append(f"    goto L_{after:04x};")
            self.need_labels.add(after)
            skip.update(a for a in order if entry <= a < after)
        for idx, a in enumerate(order):
            if a in skip:
                continue
            insn = body.insns[a]
            nxt = order[idx + 1] if idx + 1 < len(order) else None
            if a in self.need_labels:
                lines.append(f"L_{a:04x}:")
            # fuse flag-setting instruction + conditional jump
            fused = self.try_fuse(insn, nxt)
            if fused:
                lines += [f"    {l}" for l in fused]
                skip.add(nxt)
                insn = body.insns[nxt]
                a = nxt
                nxt = order[idx + 2] if idx + 2 < len(order) else None
            else:
                try:
                    code = self.insn(insn)
                except EmitError as e:
                    raise EmitError(f"{insn}: {e}") from None
                lines += [f"    {l}" for l in code]
            fl = body.flows[a]
            for t in fl.targets if fl.kind in ("jcc", "jmp") else ():
                if t in body.invalid:
                    trap_labels.add(t)
            if fl.falls_through and insn.end != nxt:
                if insn.end in body.invalid or insn.end not in body.insns:
                    trap_labels.add(insn.end)
                lines.append(f"    goto L_{insn.end:04x};")
                self.need_labels.add(insn.end)
        for t in sorted(trap_labels):
            lines.append(f"L_{t:04x}:")
            lines.append(f'    rt_trap(c, "undecodable code", {self.sel(self.seg)}, {t:#x});')
        # declare only labels that are defined (all referenced labels are)
        return lines

    def try_fuse(self, insn, nxt_off):
        """CMP/TEST/arith immediately followed by a Jcc whose flags are dead afterwards."""
        if nxt_off is None or nxt_off in self.need_labels or nxt_off != insn.end:
            return None
        j = self.body.insns[nxt_off]
        if j.mnem not in JCC_COND or j.mnem in ("jo", "jno", "jp", "jnp"):
            return None
        uses, defs, may = F.uses_defs(insn)
        juses = F.JCC_USES[j.mnem]
        if (self.live[insn.off] & ~juses) or (self.live[nxt_off] & (defs | may)):
            return None
        m = insn.mnem
        ops = insn.cs.operands
        if m in ("cmp", "sub") and len(ops) == 2:
            table = CMP_COND
        elif m in ("test", "and", "or", "xor"):
            table = LOGIC_COND
        elif m in ("add", "inc", "dec", "neg"):
            table = RESULT_COND
        else:
            return None
        if j.mnem not in table:
            return None
        size = ops[0].size
        T, S = CT[size], ST[size]
        cond = table[j.mnem].replace("(S)", f"({S})").replace("(T)", f"({T})")
        target = j.cs.operands[0].imm & 0xFFFF
        out = ["{"]
        if m in ("cmp", "sub"):
            out.append(f"    {T} a = {self.read(insn, ops[0])}, b = {self.read(insn, ops[1])};")
            if m == "sub":
                out.append(f"    {self.write(insn, ops[0], f'({T})(a - b)')}")
        else:
            out += [f"    {l}" for l in self.result_only(insn, size)]
        out.append(f"    if ({cond}) goto L_{target:04x};")
        out.append("}")
        return out

    def result_only(self, insn, size):
        """Compute r (and write the destination) for logic/add/inc/dec/neg without flags."""
        T = CT[size]
        ops = insn.cs.operands
        m = insn.mnem
        dst = ops[0]
        if m == "test":
            return [f"{T} r = ({T})({self.read(insn, ops[0])} & {self.read(insn, ops[1])});"]
        if m in ("and", "or", "xor"):
            op = {"and": "&", "or": "|", "xor": "^"}[m]
            return [f"{T} r = ({T})({self.read(insn, ops[0])} {op} {self.read(insn, ops[1])});",
                    self.write(insn, dst, "r")]
        if m == "add":
            return [f"{T} r = ({T})({self.read(insn, ops[0])} + {self.read(insn, ops[1])});",
                    self.write(insn, dst, "r")]
        if m in ("inc", "dec"):
            return [f"{T} r = ({T})({self.read(insn, dst)} {'+' if m == 'inc' else '-'} 1);",
                    self.write(insn, dst, "r")]
        if m == "neg":
            return [f"{T} r = ({T})(0 - {self.read(insn, dst)});", self.write(insn, dst, "r")]
        raise EmitError("not fusable")

    def need(self, insn):
        uses, defs, may = F.uses_defs(insn)
        return (defs | may) & self.live[insn.off]

    # -- instruction dispatch ----------------------------------------------------------------
    def insn(self, insn):
        m = insn.mnem
        ops = insn.cs.operands
        op32 = 0x66 in insn.cs.prefix
        base = m.split()[-1]
        here = self.sel(self.seg)
        fl = self.body.flows[insn.off]

        if m in ("nop", "wait"):
            return []
        if m == "mov":
            if ops[0].type == X86_OP_REG and insn.cs.reg_name(ops[0].reg) in SREGS:
                return [self.write(insn, ops[0], f"(uint16_t){self.read(insn, ops[1])}")]
            return [self.write(insn, ops[0], self.read(insn, ops[1]))]
        if m in ("add", "sub", "cmp", "adc", "sbb", "and", "or", "xor", "test"):
            return self.alu(insn)
        if m in ("inc", "dec", "neg", "not"):
            return self.unary(insn)
        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            return self.shift(insn)
        if m in ("mul", "imul", "div", "idiv"):
            return self.muldiv(insn)
        if m == "lea":
            seg, off = self.address(insn, ops[1])
            return [self.write(insn, ops[0], off)]
        if m in ("les", "lds", "lss", "lfs", "lgs"):
            seg, off = self.address(insn, ops[1])
            sreg = m[1:]
            if ops[0].size == 4:
                return [f"{{ uint32_t o = rd32({seg}, {off}); uint16_t s = rd16({seg}, (uint16_t)({off} + 4));",
                        f"  {self.reg(insn, ops[0].reg)[0]} = o; c->{sreg} = s; }}"]
            return [f"{{ uint16_t o = rd16({seg}, {off}); uint16_t s = rd16({seg}, (uint16_t)({off} + 2));",
                    f"  {self.reg(insn, ops[0].reg)[0]} = o; c->{sreg} = s; }}"]
        if m == "xchg":
            size = ops[0].size
            return [f"{{ {CT[size]} t = {self.read(insn, ops[0])};",
                    f"  {self.write(insn, ops[0], self.read(insn, ops[1]))}",
                    f"  {self.write(insn, ops[1], 't')} }}"]
        if m == "push":
            if ops[0].type == X86_OP_REG and insn.cs.reg_name(ops[0].reg) in SREGS:
                return [f"push16(c, {self.read(insn, ops[0])});"]
            size = ops[0].size if ops[0].type != X86_OP_IMM else (4 if op32 else 2)
            val = self.read(insn, ops[0]) if ops[0].type != X86_OP_IMM else self.imm(insn, ops[0].imm, size)
            return [f"{{ {CT[size]} v = {val}; push{BITS[size]}(c, v); }}"]
        if m == "pop":
            if ops[0].type == X86_OP_REG and insn.cs.reg_name(ops[0].reg) in SREGS:
                return [self.write(insn, ops[0], "pop16(c)")]
            size = ops[0].size
            return [f"{{ {CT[size]} v = pop{BITS[size]}(c); {self.write(insn, ops[0], 'v')} }}"]
        if base in ("pushf", "pushfd"):
            return ["push32(c, flags_pack(c));" if op32 else "push16(c, flags_pack(c));"]
        if base in ("popf", "popfd"):
            return ["flags_unpack(c, (uint16_t)pop32(c));" if op32 else "flags_unpack(c, pop16(c));"]
        if insn.cs.opcode[0] == 0x98:   # capstone names these by the wrong operand size
            return ["c->a.e = (uint32_t)(int32_t)(int16_t)c->a.x;" if op32 else "c->a.x = (uint16_t)(int16_t)(int8_t)c->a.b.l;"]
        if insn.cs.opcode[0] == 0x99:
            return ["c->d.e = (c->a.e & 0x80000000u) ? 0xFFFFFFFFu : 0;" if op32 else "c->d.x = (c->a.x & 0x8000) ? 0xFFFF : 0;"]
        if m == "enter":
            if ops[1].imm != 0:
                raise EmitError("enter with nesting level")
            return ["push16(c, c->bp.x);", "c->bp.x = c->sp.x;", f"c->sp.x -= {ops[0].imm:#x};"]
        if m == "leave":
            return ["c->sp.x = c->bp.x;", "c->bp.x = pop16(c);"]
        if m == "xlatb":
            seg = self.seg_override(insn) or "ds"
            segexpr = here if seg == "cs" else f"c->{seg}"
            return [f"c->a.b.l = rd8({segexpr}, (uint16_t)(c->b.x + c->a.b.l));"]
        if m in ("clc", "stc", "cmc"):
            return [{"clc": "c->cf = 0;", "stc": "c->cf = 1;", "cmc": "c->cf ^= 1;"}[m]]
        if m in ("cld", "std"):
            return [f"c->df = {int(m == 'std')};"]
        if m in ("cli", "sti"):
            return [f"c->ifl = {int(m == 'sti')};"]
        if m == "lahf":
            return ["c->a.b.h = (uint8_t)(c->sf << 7 | c->zf << 6 | c->af << 4 | c->pf << 2 | 2 | c->cf);"]
        if m == "sahf":
            return ["{ uint8_t f = c->a.b.h; c->sf = f >> 7 & 1; c->zf = f >> 6 & 1; c->af = f >> 4 & 1;",
                    "  c->pf = f >> 2 & 1; c->cf = f & 1; }"]
        if m in ("lar", "lsl"):
            return [f"{{ uint16_t v = {self.reg(insn, ops[0].reg)[0]}; rt_{m}(c, &v, {self.read(insn, ops[1])});",
                    f"  {self.reg(insn, ops[0].reg)[0]} = v; }}"]
        if m in ("verr", "verw"):
            return [f"rt_{m}(c, {self.read(insn, ops[0])});"]
        if m == "int":
            return [f"rt_int(c, {ops[0].imm:#x});"]
        if base in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsw", "lodsd",
                    "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd"):
            return self.string(insn, base)
        if insn.cs.opcode[0] in range(0xD8, 0xE0):
            return self.fpu(insn)
        if fl.kind in ("jcc", "jmp", "switch", "ijmp", "call", "ret", "trap", "tail"):
            return self.control(insn, fl)
        raise EmitError("unsupported instruction")

    # -- integer ALU --------------------------------------------------------------------------
    def alu(self, insn):
        m = insn.mnem
        ops = insn.cs.operands
        size = ops[0].size
        T = CT[size]
        need = self.need(insn)
        kind = {"add": "add", "adc": "add", "sub": "sub", "sbb": "sub", "cmp": "sub",
                "and": "logic", "or": "logic", "xor": "logic", "test": "logic"}[m]
        cin = m in ("adc", "sbb")
        # xor r, r / sub r, r: zeroing idiom
        if m in ("xor", "sub") and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG and ops[0].reg == ops[1].reg:
            out = [self.write(insn, ops[0], "0")]
            if need:
                out = ["{", f"    {T} a = 0, b = 0, r = 0;", f"    {out[0]}"] + \
                      [f"    {l}" for l in flag_code(kind, need, size)] + ["    (void)a; (void)b; (void)r;", "}"]
            return out
        op = {"add": "+", "adc": "+", "sub": "-", "sbb": "-", "cmp": "-", "and": "&", "or": "|", "xor": "^", "test": "&"}[m]
        expr = f"a {op} b" + (f" {op} cin" if cin else "")
        out = ["{", f"    {T} a = {self.read(insn, ops[0])}, b = {self.read(insn, ops[1])};"]
        if cin:
            out.append("    uint8_t cin = c->cf;")
        out.append(f"    {T} r = ({T})({expr});")
        if m not in ("cmp", "test"):
            out.append(f"    {self.write(insn, ops[0], 'r')}")
        out += [f"    {l}" for l in flag_code(kind, need, size, cin)]
        out.append("    (void)a; (void)b; (void)r;")
        out.append("}")
        return out

    def unary(self, insn):
        m = insn.mnem
        dst = insn.cs.operands[0]
        size = dst.size
        T = CT[size]
        if m == "not":
            return [self.write(insn, dst, f"({T})~{self.read(insn, dst)}")]
        need = self.need(insn)
        expr = {"inc": "a + 1", "dec": "a - 1", "neg": "0 - a"}[m]
        out = ["{", f"    {T} a = {self.read(insn, dst)};", f"    {T} r = ({T})({expr});",
               f"    {self.write(insn, dst, 'r')}"]
        out += [f"    {l}" for l in flag_code(m, need, size)]
        out += ["    (void)a;", "}"]
        return out

    def shift(self, insn):
        m = insn.mnem
        ops = insn.cs.operands
        dst = ops[0]
        size = dst.size
        T, S, W = CT[size], ST[size], BITS[size]
        sign = SIGN[size]
        if len(ops) < 2 or (ops[1].type == X86_OP_IMM and ops[1].imm == 0 and insn.cs.opcode[0] in (0xD0, 0xD1)):
            count = "1"                          # D0/D1 forms: implicit count of 1
        elif ops[1].type == X86_OP_IMM:
            count = str(ops[1].imm & 31)
        else:
            count = "(c->c.b.l & 31)"
        if count == "0":
            return []
        need = self.need(insn)
        out = ["{", f"    unsigned n = {count};", f"    {T} a = {self.read(insn, dst)}, r;"]
        body = []
        if m in ("shl", "sal"):
            body.append(f"uint64_t t = (uint64_t)a << n; r = ({T})t;")
            fl = {F.CF: f"c->cf = (t >> {W}) & 1;", F.OF: f"c->of = ((r & {sign}) != 0) ^ ((t >> {W}) & 1);"}
        elif m == "shr":
            body.append(f"r = ({T})((uint64_t)a >> n);")
            fl = {F.CF: "c->cf = ((uint64_t)a >> (n - 1)) & 1;", F.OF: f"c->of = (a & {sign}) != 0;"}
        elif m == "sar":
            body.append(f"r = ({T})((int64_t)({S})a >> n);")
            fl = {F.CF: f"c->cf = ((int64_t)({S})a >> (n - 1)) & 1;", F.OF: "c->of = 0;"}
        elif m in ("rol", "ror"):
            body.append(f"unsigned k = n % {W};")
            if m == "rol":
                body.append(f"r = k ? ({T})((a << k) | (a >> ({W} - k))) : a;")
                fl = {F.CF: "c->cf = r & 1;", F.OF: f"c->of = ((r & {sign}) != 0) ^ (r & 1);"}
            else:
                body.append(f"r = k ? ({T})((a >> k) | (a << ({W} - k))) : a;")
                fl = {F.CF: f"c->cf = (r & {sign}) != 0;",
                      F.OF: f"c->of = (((r ^ (r << 1)) & {sign}) != 0);"}
        else:   # rcl / rcr
            body.append(f"r = ({T})rotate_carry(c, a, n, {W}, {int(m == 'rcl')});")
            fl = {}
        for bit, code in fl.items():
            if need & bit:
                body.append(code)
        if m in ("shl", "sal", "shr", "sar"):
            for bit, code in ((F.ZF, "c->zf = r == 0;"), (F.SF, f"c->sf = (r & {sign}) != 0;"),
                              (F.PF, "c->pf = parity8(r);"), (F.AF, "c->af = 0;")):
                if need & bit:
                    body.append(code)
        if count.startswith("("):
            out.append("    if (n) {")
            out += [f"        {l}" for l in body]
            out.append(f"        {self.write(insn, dst, 'r')}")
            out.append("    }")
        else:
            out += [f"    {l}" for l in body]
            out.append(f"    {self.write(insn, dst, 'r')}")
        out += ["    (void)a;", "}"]
        return out

    def muldiv(self, insn):
        m = insn.mnem
        ops = insn.cs.operands
        need = self.need(insn)
        here = f"{self.sel(self.seg)}, {insn.off:#x}"
        if m == "imul" and len(ops) == 3:
            size = ops[0].size
            S, W = ST[size], BITS[size]
            wide = "int64_t" if size == 4 else "int32_t"
            out = [f"{{ {wide} r = ({wide})({S}){self.read(insn, ops[1])} * ({wide})({S}){self.imm(insn, ops[2].imm, size)};",
                   f"  {self.write(insn, ops[0], f'({CT[size]})r')}"]
            if need & (F.CF | F.OF):
                out.append(f"  c->cf = c->of = r != ({S})r;")
            return out + ["}"]
        if len(ops) != 1:
            raise EmitError("unsupported mul/div form")
        size = ops[0].size
        src = self.read(insn, ops[0])
        if m in ("mul", "imul"):
            signed = m == "imul"
            if size == 1:
                prod = f"(int16_t)(int8_t)c->a.b.l * (int8_t)s" if signed else "(uint16_t)(c->a.b.l * s)"
                out = [f"{{ uint8_t s = {src}; uint16_t r = (uint16_t)({prod}); c->a.x = r;"]
                ovf = "(int16_t)r != (int8_t)r" if signed else "(r >> 8) != 0"
            elif size == 2:
                prod = "(int32_t)(int16_t)c->a.x * (int16_t)s" if signed else "(uint32_t)c->a.x * s"
                out = [f"{{ uint16_t s = {src}; uint32_t r = (uint32_t)({prod}); c->a.x = (uint16_t)r; c->d.x = (uint16_t)(r >> 16);"]
                ovf = "(int32_t)r != (int16_t)r" if signed else "(r >> 16) != 0"
            else:
                prod = "(int64_t)(int32_t)c->a.e * (int32_t)s" if signed else "(uint64_t)c->a.e * s"
                out = [f"{{ uint32_t s = {src}; uint64_t r = (uint64_t)({prod}); c->a.e = (uint32_t)r; c->d.e = (uint32_t)(r >> 32);"]
                ovf = "(int64_t)r != (int32_t)r" if signed else "(r >> 32) != 0"
            if need & (F.CF | F.OF):
                out.append(f"  c->cf = c->of = {ovf};")
            return out + ["}"]
        # div / idiv
        if m == "div":
            if size == 1:
                return [f"{{ uint8_t d = {src}; uint16_t n = c->a.x;",
                        f"  if (!d || n / d > 0xFF) rt_divide_error(c, {here});",
                        "  c->a.b.l = (uint8_t)(n / d); c->a.b.h = (uint8_t)(n % d); }"]
            if size == 2:
                return [f"{{ uint16_t d = {src}; uint32_t n = (uint32_t)c->d.x << 16 | c->a.x;",
                        f"  if (!d || n / d > 0xFFFF) rt_divide_error(c, {here});",
                        "  c->a.x = (uint16_t)(n / d); c->d.x = (uint16_t)(n % d); }"]
            return [f"{{ uint32_t d = {src}; uint64_t n = (uint64_t)c->d.e << 32 | c->a.e;",
                    f"  if (!d || n / d > 0xFFFFFFFFu) rt_divide_error(c, {here});",
                    "  c->a.e = (uint32_t)(n / d); c->d.e = (uint32_t)(n % d); }"]
        if size == 1:
            return [f"{{ int8_t d = (int8_t){src}; int16_t n = (int16_t)c->a.x;",
                    f"  if (!d || (n == -32768 && d == -1)) rt_divide_error(c, {here});",
                    "  int16_t q = n / d;",
                    f"  if (q > 127 || q < -128) rt_divide_error(c, {here});",
                    "  c->a.b.l = (uint8_t)q; c->a.b.h = (uint8_t)(n % d); }"]
        if size == 2:
            return [f"{{ int16_t d = (int16_t){src}; int32_t n = (int32_t)((uint32_t)c->d.x << 16 | c->a.x);",
                    f"  if (!d || (n == INT32_MIN && d == -1)) rt_divide_error(c, {here});",
                    "  int32_t q = n / d;",
                    f"  if (q > 32767 || q < -32768) rt_divide_error(c, {here});",
                    "  c->a.x = (uint16_t)q; c->d.x = (uint16_t)(n % d); }"]
        return [f"{{ int32_t d = (int32_t){src}; int64_t n = (int64_t)((uint64_t)c->d.e << 32 | c->a.e);",
                f"  if (!d || (n == INT64_MIN && d == -1)) rt_divide_error(c, {here});",
                "  int64_t q = n / d;",
                f"  if (q > INT32_MAX || q < INT32_MIN) rt_divide_error(c, {here});",
                "  c->a.e = (uint32_t)q; c->d.e = (uint32_t)(n % d); }"]

    # -- strings ------------------------------------------------------------------------------
    def string(self, insn, base):
        size = {"b": 1, "w": 2, "d": 4}[base[-1]]
        op = base[:-1]
        pfx = insn.cs.prefix[0]
        seg = self.seg_override(insn) or "ds"
        segexpr = self.sel(self.seg) if seg == "cs" else f"c->{seg}"
        a32 = "32" if insn.cs.addr_size == 4 else ""
        if a32 and op not in ("movs", "lods", "stos"):
            raise EmitError("32-bit addressed string instruction")
        if op in ("movs", "lods"):
            return [f"str_{op}{a32}(c, {size}, {segexpr}, {int(pfx == 0xF3)});"]
        if op == "stos":
            return [f"str_stos{a32}(c, {size}, {int(pfx == 0xF3)});"]
        rep = {0xF3: 1, 0xF2: 2}.get(pfx, 0)
        if op == "scas":
            return [f"str_scas(c, {size}, {rep});"]
        return [f"str_cmps(c, {size}, {segexpr}, {rep});"]

    # -- control flow ------------------------------------------------------------------------
    def control(self, insn, fl):
        m = insn.mnem
        ops = insn.cs.operands
        here = self.sel(self.seg)
        ret_ip = f"{insn.end:#x}"
        if fl.kind == "jcc":
            t = fl.targets[0]
            if m in JCC_COND:
                return [f"if ({JCC_COND[m]}) goto L_{t:04x};"]
            if m == "jcxz":
                return [f"if (c->c.x == 0) goto L_{t:04x};"]
            if m == "jecxz":
                return [f"if (c->c.e == 0) goto L_{t:04x};"]
            reg = "c->c.e" if insn.cs.addr_size == 4 else "c->c.x"
            cond = {"loop": f"--{reg} != 0", "loope": f"--{reg} != 0 && c->zf",
                    "loopne": f"--{reg} != 0 && !c->zf"}[m]
            return [f"if ({cond}) goto L_{t:04x};"]
        if fl.kind == "jmp":
            return [f"goto L_{fl.targets[0]:04x};"]
        if fl.kind in ("switch", "ijmp") and m == "jmp":
            cases = fl.targets if fl.kind == "switch" else sorted(l for l in self.body.labels if l in self.body.insns)
            cases = [t for t in cases if t in self.body.insns]
            out = [f"{{ uint16_t t = (uint16_t){self.read(insn, ops[0])};", "switch (t) {"]
            out += [f"case {t:#x}: goto L_{t:04x};" for t in sorted(set(cases))]
            ret_check = "if (t == ret_ip) return; " if fl.kind == "ijmp" else ""
            out.append(f"default: {ret_check}rt_call_near(c, {here}, t); return;")
            out.append("} }")
            return out
        if fl.kind == "ijmp":       # ljmp m16:16
            seg, off = self.address(insn, ops[0])
            return [f"{{ uint16_t o = rd16({seg}, {off}), s = rd16({seg}, (uint16_t)({off} + 2));",
                    f"  if (o == ret_ip && s == ret_cs) return;",
                    "  rt_call_far(c, s, o); return; }"]
        if fl.kind == "tail":
            return self.call_target(fl.call_target, insn, push=False) + ["return;"]
        if fl.kind == "call":
            tgt = fl.call_target
            out = []
            if tgt is None:
                if m == "call":
                    out = [f"{{ uint16_t t = {self.read(insn, ops[0])}; push16(c, {ret_ip}); rt_call_near(c, {here}, t); }}"]
                else:
                    seg, off = self.address(insn, ops[0])
                    out = [f"{{ uint16_t o = rd16({seg}, {off}), s = rd16({seg}, (uint16_t)({off} + 2));",
                           f"  push16(c, {here}); push16(c, {ret_ip}); rt_call_far(c, s, o); }}"]
            else:
                if m == "lcall" or isinstance(tgt, Import):
                    out.append(f"push16(c, {here});")
                out.append(f"push16(c, {ret_ip});")
                call = self.call_target(tgt, insn, push=True)
                if self.is_setjmp(tgt):
                    out.append("{ uint32_t key = rd32(c->ss, (uint16_t)(c->sp.x + 4));")
                    out.append(f"  if (setjmp(*rt_jmp_slot(c, key)) == 0) {{ {' '.join(call)} }} }}")
                else:
                    out += call
            if not fl.falls_through:
                out.append(f'rt_trap(c, "returned from no-return call", {here}, {insn.off:#x});')
            return out
        if fl.kind == "ret":
            n = ops[0].imm if ops else 0
            if m == "ret":
                return [f"c->sp.x += {2 + n};", "return;"]
            if m == "retf":
                if self.longjmp_like:
                    return [f"c->sp.x += {4 + n};", "rt_longjmp(c, jmp_key);"]
                return [f"c->sp.x += {4 + n};", "return;"]
            raise EmitError("iret")
        if fl.kind == "trap":
            return [f'rt_trap(c, "{m}", {here}, {insn.off:#x});']
        raise EmitError(f"unhandled control flow {fl.kind}")

    def is_setjmp(self, tgt):
        if isinstance(tgt, Internal):
            return self.prog.functions.get((tgt.seg, tgt.off)) in SETJMP_FUNCS
        return isinstance(tgt, Import) and (tgt.module, tgt.ordinal) in SETJMP_IMPORTS

    def call_target(self, tgt, insn, push):
        if isinstance(tgt, Internal):
            return [f"{self.fname(tgt.seg, tgt.off)}(c);"]
        if isinstance(tgt, Import):
            return [f"IMPCALL({self.imports(tgt)}); /* {tgt} */"]
        raise EmitError("far call/jmp without relocation")

    # -- x87 ----------------------------------------------------------------------------------
    def fpu(self, insn):
        op = insn.cs.opcode[0]
        modrm = insn.cs.modrm
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        if mod != 3:
            memop = next(o for o in insn.cs.operands if o.type == X86_OP_MEM)
            seg, off = self.address(insn, memop)
            loc = f"{seg}, {off}"
            arith = ["+", "*", "com", "comp", "-", "-r", "/", "/r"]
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                src = {0xD8: f"fpu_rd_f32({loc})", 0xDC: f"fpu_rd_f64({loc})",
                       0xDA: f"(double)(int32_t)rd32({loc})", 0xDE: f"(double)(int16_t)rd16({loc})"}[op]
                return self.fpu_arith(arith[reg], "ST(0)", src, pop=False)
            table = {
                (0xD9, 0): [f"fpu_push(c, fpu_rd_f32({loc}));"],
                (0xD9, 2): [f"fpu_wr_f32({loc}, ST(0));"],
                (0xD9, 3): [f"fpu_wr_f32({loc}, ST(0));", "fpu_pop(c);"],
                (0xD9, 5): [f"c->fpu.cw = rd16({loc});"],
                (0xD9, 6): [f"fpu_store_env(c, {loc});"],
                (0xD9, 7): [f"wr16({loc}, c->fpu.cw);"],
                (0xDD, 0): [f"fpu_push(c, fpu_rd_f64({loc}));"],
                (0xDD, 2): [f"fpu_wr_f64({loc}, ST(0));"],
                (0xDD, 3): [f"fpu_wr_f64({loc}, ST(0));", "fpu_pop(c);"],
                (0xDD, 7): [f"wr16({loc}, fpu_status(c));"],
                (0xDB, 0): [f"fpu_push(c, (double)(int32_t)rd32({loc}));"],
                (0xDB, 2): [f"fpu_wr_int(c, {loc}, ST(0), 4);"],
                (0xDB, 3): [f"fpu_wr_int(c, {loc}, ST(0), 4);", "fpu_pop(c);"],
                (0xDB, 5): [f"fpu_push(c, fpu_rd_f80({loc}));"],
                (0xDB, 7): [f"fpu_wr_f80({loc}, ST(0));", "fpu_pop(c);"],
                (0xDF, 0): [f"fpu_push(c, (double)(int16_t)rd16({loc}));"],
                (0xDF, 2): [f"fpu_wr_int(c, {loc}, ST(0), 2);"],
                (0xDF, 3): [f"fpu_wr_int(c, {loc}, ST(0), 2);", "fpu_pop(c);"],
                (0xDF, 5): [f"{{ uint64_t v = (uint64_t)rd32({loc}) | (uint64_t)rd32({seg}, (uint16_t)({off} + 4)) << 32;",
                            "  fpu_push(c, (double)(int64_t)v); }"],
                (0xDF, 7): [f"fpu_wr_int(c, {loc}, ST(0), 8);", "fpu_pop(c);"],
            }
            if (op, reg) in table:
                return table[(op, reg)]
            raise EmitError(f"unsupported x87 memory form {op:#x}/{reg}")
        i = rm
        if op == 0xD8:
            return self.fpu_arith(["+", "*", "com", "comp", "-", "-r", "/", "/r"][reg], "ST(0)", f"ST({i})", pop=False)
        if op == 0xDC and reg not in (2, 3):
            # DC forms write ST(i); Intel's DC E0/E8 and F0/F8 are FSUBR/FSUB and FDIVR/FDIV
            return self.fpu_arith(["+", "*", None, None, "-r", "-", "/r", "/"][reg], f"ST({i})", "ST(0)", pop=False)
        if op == 0xDE:
            if reg == 3 and i == 1:
                return ["fpu_compare(c, ST(0), ST(1));", "fpu_pop(c);", "fpu_pop(c);"]   # fcompp
            return self.fpu_arith(["+", "*", "com", "comp", "-r", "-", "/r", "/"][reg], f"ST({i})", "ST(0)", pop=True)
        if op == 0xD9:
            if reg == 0:
                return [f"{{ double v = ST({i}); fpu_push(c, v); }}"]
            if reg == 1:
                return [f"{{ double t = ST(0); ST(0) = ST({i}); ST({i}) = t; }}"]
            simple = {0xE0: "ST(0) = -ST(0);", 0xE1: "ST(0) = ST(0) < 0 ? -ST(0) : ST(0);",
                      0xE4: "fpu_compare(c, ST(0), 0.0);", 0xE5: "fpu_examine(c);",
                      0xE8: "fpu_push(c, 1.0);", 0xE9: "fpu_push(c, 3.321928094887362347870);",
                      0xEA: "fpu_push(c, 1.442695040888963407360);", 0xEB: "fpu_push(c, 3.141592653589793238463);",
                      0xEC: "fpu_push(c, 0.301029995663981195214);", 0xED: "fpu_push(c, 0.693147180559945309417);",
                      0xEE: "fpu_push(c, 0.0);"}
            if modrm in simple:
                return [simple[modrm]]
            named = {0xF0: "f2xm1", 0xF1: "fyl2x", 0xF2: "fptan", 0xF3: "fpatan", 0xF4: "fxtract",
                     0xF5: "fprem1", 0xF8: "fprem", 0xF9: "fyl2xp1", 0xFA: "fsqrt", 0xFB: "fsincos",
                     0xFC: "frndint", 0xFD: "fscale", 0xFE: "fsin", 0xFF: "fcos"}
            if modrm in named:
                return [f'fpu_op(c, "{named[modrm]}");']
        if op == 0xDD:
            if reg == 2:
                return [f"ST({i}) = ST(0);"]
            if reg == 3:
                return [f"ST({i}) = ST(0);", "fpu_pop(c);"]
        if op == 0xDB and modrm == 0xE2:
            return ["c->fpu.sw &= 0x7F00;"]                 # fnclex
        if op == 0xDB and modrm == 0xE3:
            return ["fpu_init(c);"]
        if op == 0xDF and modrm == 0xE0:
            return ["c->a.x = fpu_status(c);"]
        raise EmitError(f"unsupported x87 register form {op:#x} {modrm:#x}")

    def fpu_arith(self, kind, dst, src, pop):
        if kind in ("com", "comp"):
            out = [f"fpu_compare(c, {dst}, {src});"]
            if kind == "comp":
                out.append("fpu_pop(c);")
            return out
        expr = {"+": f"{dst} + {src}", "*": f"{dst} * {src}", "-": f"{dst} - {src}",
                "-r": f"{src} - {dst}", "/": f"{dst} / {src}", "/r": f"{src} / {dst}"}[kind]
        out = [f"{dst} = {expr};"]
        if pop:
            out.append("fpu_pop(c);")
        return out
