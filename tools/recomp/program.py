"""Decoding and control-flow discovery.

Each function is compiled from its own entry point: its body is every instruction reachable
from the entry through fall-through, jumps and switch cases. Code reachable from several
entries (shared tails, jumps into another function's middle, the C runtime's multi-entry
assembly) is simply duplicated into each body, so a body never needs to jump into another
compiled function. Calls go to function entries; indirect transfers go through a runtime
dispatcher.

Instructions are keyed by (segment, offset), and every reached offset is decoded on its own,
so overlapping instruction streams are fine.
"""
import json
from dataclasses import dataclass, field

from capstone import CS_ARCH_X86, CS_MODE_16, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

from .ne import FARPTR, SELECTOR, Import, Internal, NEModule

CONDITIONAL_JUMPS = {
    "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge",
    "jle", "jg", "jcxz", "jecxz", "loop", "loope", "loopne",
}
NORETURN_IMPORTS = {("KERNEL", 1), ("KERNEL", 137)}     # FatalExit, FatalAppExit


@dataclass
class Insn:
    seg: int
    off: int
    size: int
    mnem: str
    op_str: str
    cs: object               # capstone CsInsn (detail enabled)
    fixups: list

    @property
    def end(self):
        return self.off + self.size

    def __str__(self):
        return f"{self.seg}:{self.off:04x} {self.mnem} {self.op_str}"


@dataclass
class Flow:
    """Control-flow effect of one instruction."""
    kind: str                # "next", "jcc", "jmp", "switch", "ijmp", "call", "ret", "trap", "tail"
    targets: tuple = ()      # intra-segment offsets (jcc/jmp/switch) or call targets
    falls_through: bool = True
    call_target: object = None   # Internal / Import / None (indirect) for calls and far jumps
    possible: tuple = ()         # indirect calls: the Internal targets it can reach, if known


@dataclass
class Body:
    entry: tuple             # (seg, off)
    name: str
    insns: dict = field(default_factory=dict)      # off -> Insn
    flows: dict = field(default_factory=dict)      # off -> Flow
    labels: set = field(default_factory=set)       # offsets that are jump/switch targets
    invalid: set = field(default_factory=set)      # offsets that failed to decode


class Program:
    def __init__(self, exe_path, analysis_path):
        self.ne = NEModule(exe_path)
        self.analysis = json.load(open(analysis_path))
        self.md = Cs(CS_ARCH_X86, CS_MODE_16)
        self.md.detail = True
        self._cache = {}
        self.ignored_segments = set(self.analysis.get("ignored_segments", []))

        self.functions = {}       # (seg, off) -> name
        self.noreturn = set()
        for f in self.analysis["functions"]:
            self.functions[(f["seg"], f["start"])] = f["name"]
            if f["noreturn"]:
                self.noreturn.add((f["seg"], f["start"]))
        for ordinal, (seg, off) in self.ne.entries.items():
            if seg not in self.ignored_segments and self.ne.segments[seg].is_code:
                self.functions.setdefault((seg, off), f"export_{ordinal}")
        self.switches = {(s["seg"], s["jump"]): s for s in self.analysis["switches"]}

    def all_bodies(self):
        """Discover every function body, adding direct call/jump targets as entries.

        A direct call can land in the middle of another function (multi-entry assembly);
        such targets get their own bodies. Then code addresses that the program builds as
        far pointers (see pointer_entries) become entries too, until nothing new turns up.
        """
        bodies = {}
        work = list(self.functions)
        while work:
            while work:
                entry = work.pop()
                if entry in bodies:
                    continue
                body = bodies[entry] = self.discover(*entry)
                for fl in body.flows.values():
                    for t in (fl.call_target,) + fl.possible:
                        if isinstance(t, Internal) and t.off is not None and (t.seg, t.off) not in self.functions:
                            self.functions[(t.seg, t.off)] = f"sub_{t.seg}_{t.off:04x}"
                            work.append((t.seg, t.off))
            for entry in self.pointer_entries(bodies):
                self.functions[entry] = f"ptr_{entry[0]}_{entry[1]:04x}"
                work.append(entry)
        return bodies

    # -- far pointers built from a relocated selector and an immediate offset --------------
    def pointer_entries(self, bodies):
        """Code addresses the program assembles as far pointers at run time.

        A far pointer to code needs a relocation only for its selector; the offset is a plain
        immediate. The shapes compiled code uses: `mov [x], off` / `mov [x+2], seg`, and
        `push seg` / `push off`. Candidates must be instruction starts of existing bodies or
        decode cleanly from scratch; everything returned is a new entry.
        """
        boundaries = {}                    # seg -> set of instruction offsets in any body
        for b in bodies.values():
            boundaries.setdefault(b.entry[0], set()).update(b.insns)
        found = set()
        for b in bodies.values():
            seg = b.entry[0]
            order = sorted(b.insns)
            for i, a in enumerate(order):
                insn = b.insns[a]
                tseg = self._selector_imm(insn)
                if tseg is None:
                    continue
                for cand in self._paired_offsets(b, order, i, insn):
                    if self._plausible_entry(tseg, cand, boundaries):
                        found.add((tseg, cand))
        return sorted(e for e in found if e not in self.functions)

    def _selector_imm(self, insn):
        """Code segment number if `insn` stores a relocated code selector as an immediate."""
        ops = insn.cs.operands
        if not ops or ops[-1].type != X86_OP_IMM or insn.mnem not in ("mov", "push"):
            return None
        for f in insn.fixups:
            if f.kind == SELECTOR and isinstance(f.target, Internal) and f.target.off is None:
                t = self.ne.segments.get(f.target.seg)
                if t is not None and t.is_code and f.target.seg not in self.ignored_segments:
                    return f.target.seg
        return None

    @staticmethod
    def _mem_key(op):
        m = op.mem
        return (op.size, m.segment, m.base, m.index, m.scale)

    def _paired_offsets(self, b, order, i, insn):
        ops = insn.cs.operands
        near = [b.insns[order[j]] for j in range(max(0, i - 8), min(len(order), i + 9)) if j != i]
        out = []
        if insn.mnem == "push":
            # far pointers are pushed selector first: the offset follows
            if i + 1 < len(order):
                nxt = b.insns[order[i + 1]]
                if nxt.mnem == "push" and nxt.cs.operands[0].type == X86_OP_IMM and not nxt.fixups:
                    out.append(nxt.cs.operands[0].imm & 0xFFFF)
            return out
        if ops[0].type == X86_OP_MEM and ops[0].size == 2:
            key, disp = self._mem_key(ops[0]), ops[0].mem.disp
            for o in near:
                oo = o.cs.operands
                if (o.mnem == "mov" and len(oo) == 2 and oo[0].type == X86_OP_MEM and oo[1].type == X86_OP_IMM
                        and not o.fixups and self._mem_key(oo[0]) == key and oo[0].mem.disp == disp - 2):
                    out.append(oo[1].imm & 0xFFFF)
        return out

    def _plausible_entry(self, seg, off, boundaries):
        data = self.ne.segments[seg].data
        if off >= len(data):
            return False
        if off in boundaries.get(seg, ()):
            return True                    # an instruction start of known code
        # alignment NOPs before known code
        k = off
        while k < len(data) and k - off < 16 and data[k] == 0x90:
            k += 1
        if k > off and k in boundaries.get(seg, ()):
            return True
        # unknown code: accept only if it decodes to a body with a return and no garbage
        body = self.discover(seg, off)
        overlaps = any(a not in boundaries.get(seg, ()) and any(x in boundaries.get(seg, ())
                       for x in range(a + 1, a + ins.size)) for a, ins in body.insns.items())
        return (not body.invalid and not overlaps and len(body.insns) >= 2
                and any(fl.kind == "ret" for fl in body.flows.values()))

    # -- decoding -----------------------------------------------------------------------
    def decode(self, seg, off):
        key = (seg, off)
        if key in self._cache:
            return self._cache[key]
        data = self.ne.segments[seg].data
        insn = None
        if off < len(data):
            found = next(self.md.disasm(data[off:off + 16], off, count=1), None)
            if found is not None:
                fixups = self.ne.segments[seg].fixups_in(off, off + found.size)
                insn = Insn(seg, off, found.size, found.mnemonic, found.op_str, found, fixups)
        self._cache[key] = insn
        return insn

    def far_target(self, insn):
        """Target of a direct far call/jmp (opcode 9A/EA): Internal, Import, or None."""
        off_site, sel_site = insn.off + 1, insn.off + 3
        fix = {f.site: f for f in insn.fixups}
        if off_site in fix and fix[off_site].kind == FARPTR:
            return fix[off_site].target
        if sel_site in fix and fix[sel_site].kind == SELECTOR and isinstance(fix[sel_site].target, Internal):
            data = self.ne.segments[insn.seg].data
            return Internal(fix[sel_site].target.seg, data[off_site] | data[off_site + 1] << 8)
        return None      # absolute far pointer with no relocation: unusual

    # -- control flow -------------------------------------------------------------------
    def flow(self, insn):
        m = insn.mnem
        ops = insn.cs.operands
        if m in CONDITIONAL_JUMPS:
            return Flow("jcc", (ops[0].imm & 0xFFFF,))
        if m == "jmp":
            if ops[0].type == X86_OP_IMM:
                return Flow("jmp", (ops[0].imm & 0xFFFF,), falls_through=False)
            sw = self.switches.get((insn.seg, insn.off))
            if sw:
                return Flow("switch", tuple(sw["targets"]), falls_through=False)
            return Flow("ijmp", falls_through=False)
        if m == "ljmp":
            if insn.cs.opcode[0] == 0xEA:
                return Flow("tail", falls_through=False, call_target=self.far_target(insn))
            return Flow("ijmp", falls_through=False)
        if m in ("call", "lcall"):
            if m == "call" and ops[0].type == X86_OP_IMM:
                target = Internal(insn.seg, ops[0].imm & 0xFFFF)
            elif m == "lcall" and insn.cs.opcode[0] == 0x9A:
                target = self.far_target(insn)
            else:
                target = None
            noret = (isinstance(target, Internal) and (target.seg, target.off) in self.noreturn) or \
                (isinstance(target, Import) and (target.module, target.ordinal) in NORETURN_IMPORTS)
            return Flow("call", falls_through=not noret, call_target=target)
        if m in ("ret", "retf", "iret", "iretd"):
            return Flow("ret", falls_through=False)
        if m in ("hlt", "int3", "ud2"):
            return Flow("trap", falls_through=False)
        return Flow("next")

    def discover(self, seg, off):
        """Body of the function entered at seg:off (see module docstring)."""
        body = Body((seg, off), self.functions.get((seg, off), f"sub_{seg}_{off:04x}"))
        work = [off]
        body.labels.add(off)
        while work:
            a = work.pop()
            if a in body.insns or a in body.invalid:
                continue
            insn = self.decode(seg, a)
            if insn is None:
                body.invalid.add(a)
                continue
            fl = self.flow(insn)
            body.insns[a] = insn
            body.flows[a] = fl
            if fl.kind in ("jcc", "jmp", "switch"):
                for t in fl.targets:
                    body.labels.add(t)
                    work.append(t)
            if fl.falls_through:
                work.append(insn.end)
        # a fall-through into an instruction that is also a jump target needs a label;
        # fall-through successors of branches need labels too (they start a block)
        for a, fl in body.flows.items():
            if fl.kind == "jcc":
                body.labels.add(body.insns[a].end)
        self._resolve_register_calls(body)
        return body

    # -- `mov reg, imm` ... `call reg`: a direct call in disguise ------------------------------
    _ALIASES = [{"al", "ah", "ax", "eax"}, {"bl", "bh", "bx", "ebx"}, {"cl", "ch", "cx", "ecx"},
                {"dl", "dh", "dx", "edx"}, {"si", "esi"}, {"di", "edi"}, {"bp", "ebp"}, {"sp", "esp"}]

    def _overlaps(self, a, b):
        return a == b or any(a in g and b in g for g in self._ALIASES)

    def _resolve_register_calls(self, body):
        """Turn `call reg` into a direct call when reg holds a known constant.

        The C runtime does this (`mov di, offset __searchseg` / `call di` in __fmalloc), and
        such callees return results in the flags; as indirect calls they'd get no flag
        liveness and the caller would test stale flags. A forward constant analysis of the
        register decides it per call site: `mov reg, imm` sets it, `pop reg` is taken as
        restoring a value pushed earlier, calls preserve SI/DI/BP (the MS C convention),
        anything else that writes it makes it unknown.
        """
        TOP = None

        def succs(a):
            fl, insn = body.flows[a], body.insns[a]
            out = list(fl.targets) if fl.kind in ("jcc", "jmp", "switch") else []
            if fl.falls_through:
                out.append(insn.end)
            return [t for t in out if t in body.insns]

        sites = [a for a, fl in body.flows.items()
                 if fl.kind == "call" and fl.call_target is None and body.insns[a].mnem == "call"
                 and body.insns[a].cs.operands[0].type == X86_OP_REG]
        for site in sites:
            insn = body.insns[site]
            reg = insn.cs.reg_name(insn.cs.operands[0].reg)
            callee_saved = reg in ("si", "di", "bp")

            def transfer(q, v):
                qo = q.cs.operands
                if (q.mnem == "mov" and len(qo) == 2 and qo[0].type == X86_OP_REG and qo[1].type == X86_OP_IMM
                        and q.cs.reg_name(qo[0].reg) == reg and not q.fixups):
                    return frozenset([qo[1].imm & 0xFFFF])
                if q.mnem == "pop" and qo and qo[0].type == X86_OP_REG and q.cs.reg_name(qo[0].reg) == reg:
                    return v
                if q.mnem in ("call", "lcall") and callee_saved:
                    return v
                _read, written = q.cs.regs_access()
                if any(self._overlaps(q.cs.reg_name(r), reg) for r in written):
                    return TOP
                return v

            state = {body.entry[1]: TOP}
            work = [body.entry[1]]
            known = {}
            while work:
                x = work.pop()
                v_in = state[x]
                v_out = transfer(body.insns[x], v_in)
                for t in succs(x):
                    old = state.get(t, "unset")
                    if old == "unset":
                        new_v = v_out
                    elif old is TOP or v_out is TOP:
                        new_v = TOP
                    else:
                        new_v = old | v_out
                    if old == "unset" or new_v != old:
                        state[t] = new_v
                        work.append(t)
            v = state.get(site, TOP)
            if v in (TOP, "unset"):
                continue
            targets = tuple(Internal(body.entry[0], t) for t in sorted(v))
            if len(targets) == 1:
                body.flows[site].call_target = targets[0]
            else:                         # stays a dispatched call; flag liveness sees all of them
                body.flows[site].possible = targets
