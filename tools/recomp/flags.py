"""Arithmetic flag liveness, so generated code only computes flags that are read.

Flags are tracked as x86 EFLAGS bits. "Undefined" results (e.g. ZF after MUL) count as
definitions that the generated code doesn't bother computing: a correct program can't
depend on them.

Liveness crosses direct calls: some assembly helpers return status in flags (e.g. CF from
the C runtime's DOS wrappers), and a function can pass its caller's flags through
untouched. A direct call to F reads whatever F reads on entry, and F's returns keep alive
whatever its direct callers read after the call. That is solved as a fixpoint over all
bodies.

Indirect calls are assumed to reach compiled C code (callbacks, method tables), which
never reads incoming flags or returns results in them: an indirect call reads no flags,
and its callee's returns don't keep any alive. Modelling "may call anything" instead makes
every flag live everywhere, because a few functions start with `pushf`. analyze() reports
any indirect call site that does read flags afterwards, so a violation can't go unnoticed.
"""
from capstone.x86 import X86_OP_IMM

CF, PF, AF, ZF, SF, OF = 0x001, 0x004, 0x010, 0x040, 0x080, 0x800
ALL = CF | PF | AF | ZF | SF | OF
NAMES = {CF: "cf", PF: "pf", AF: "af", ZF: "zf", SF: "sf", OF: "of"}

JCC_USES = {
    "jo": OF, "jno": OF, "jb": CF, "jae": CF, "je": ZF, "jne": ZF, "jbe": CF | ZF, "ja": CF | ZF,
    "js": SF, "jns": SF, "jp": PF, "jnp": PF, "jl": SF | OF, "jge": SF | OF, "jle": ZF | SF | OF,
    "jg": ZF | SF | OF, "loope": ZF, "loopne": ZF,
}
ARITH = {"add", "sub", "cmp", "neg", "and", "or", "xor", "test"}
SHIFTS = {"shl", "sal", "shr", "sar"}
ROTATES = {"rol", "ror", "rcl", "rcr"}


def _count_operand(insn):
    """For shifts/rotates: the count as an int, or None if it comes from CL."""
    ops = insn.cs.operands
    if len(ops) < 2:
        return 1
    if ops[1].type == X86_OP_IMM:
        return ops[1].imm & 31
    return None


def uses_defs(insn):
    """(uses, defs, may) EFLAGS masks for one instruction, ignoring calls/returns.

    `defs` are always written (killed); `may` are written only sometimes (a shift by CL
    with CL == 0, a repeated compare with CX == 0) and otherwise pass through, so they
    are neither killed nor read.
    """
    m = insn.mnem
    base = m.split()[-1]             # strip rep/repe/repne prefixes
    if m in JCC_USES:
        return JCC_USES[m], 0, 0
    if m in ARITH:
        return 0, ALL, 0
    if m in ("adc", "sbb"):
        return CF, ALL, 0
    if m in ("inc", "dec"):
        return 0, ALL & ~CF, 0
    if m in SHIFTS:
        n = _count_operand(insn)
        if n is None:
            return 0, 0, ALL
        return (0, ALL, 0) if n else (0, 0, 0)
    if m in ROTATES:
        n = _count_operand(insn)
        use = CF if m in ("rcl", "rcr") else 0
        if n is None:
            return use, 0, CF | OF
        return (use, CF | OF, 0) if n else (0, 0, 0)
    if m in ("mul", "imul", "div", "idiv"):
        return 0, ALL, 0
    if m == "cmc":
        return CF, CF, 0
    if m in ("clc", "stc"):
        return 0, CF, 0
    if m == "sahf":
        return 0, SF | ZF | AF | PF | CF, 0
    if m == "lahf":
        return SF | ZF | AF | PF | CF, 0, 0
    if base in ("pushf", "pushfd"):
        return ALL, 0, 0
    if base in ("popf", "popfd"):
        return 0, ALL, 0
    if base in ("cmpsb", "cmpsw", "cmpsd", "scasb", "scasw", "scasd"):
        return (0, 0, ALL) if m.startswith("rep") else (0, ALL, 0)
    if m in ("verr", "verw", "lar", "lsl"):
        return 0, ZF, 0
    if m == "int":
        return 0, ALL, 0             # DOS/DPMI/BIOS ignore incoming flags, may set CF etc.
    return 0, 0, 0


def analyze(prog, bodies):
    """Compute live-out flag masks for every instruction of every body.

    Returns (live, entry_live, ret_live, violations): live is {entry: {offset: mask}};
    violations lists (entry, offset, mask) for indirect calls followed by flag reads.
    `bodies` maps entry -> Body.
    """
    from .ne import Import, Internal

    entry_live = {e: 0 for e in bodies}     # flags a function reads on entry
    ret_live = {e: 0 for e in bodies}       # flags its callers read after it returns
    result = {}

    # per-instruction static info, computed once
    static = {}
    for e, body in bodies.items():
        info = {}
        for a, insn in body.insns.items():
            fl = body.flows[a]
            uses, defs, _may = uses_defs(insn)
            succ = []
            if fl.kind in ("jcc", "jmp", "switch"):
                succ += [t for t in fl.targets if t in body.insns]
            if fl.falls_through and insn.end in body.insns:
                succ.append(insn.end)
            info[a] = (fl, uses, defs, succ)
        static[e] = info

    def body_liveness(e):
        info = static[e]
        labels = [l for l in bodies[e].labels if l in info]
        # `pushf ... popf` saves and restores flags: the pushed flags matter only if they're
        # read after a popf. Without a popf in the body (e.g. `pushf; pop ax`), assume all.
        pushfs = {a for a in info if bodies[e].insns[a].mnem in ("pushf", "pushfd")}
        popfs = [a for a in info if bodies[e].insns[a].mnem in ("popf", "popfd")]
        live_in = {a: 0 for a in info}
        live_out = {a: 0 for a in info}
        order = sorted(info, reverse=True)
        changed = True
        while changed:
            changed = False
            restored = 0
            for p in popfs:
                restored |= live_out[p]
            for a in order:
                fl, uses, defs, succ = info[a]
                if a in pushfs:
                    uses = restored if popfs else ALL
                out = 0
                for s in succ:
                    out |= live_in[s]
                if fl.kind == "ret":
                    out |= ret_live[e]
                elif fl.kind in ("ijmp", "tail"):
                    # lands on a label of this body, or tail-calls compiled code that
                    # returns to our caller
                    out |= ret_live[e]
                    for l in labels:
                        out |= live_in[l]
                elif fl.kind in ("jcc", "jmp") and any(t not in info for t in fl.targets):
                    out |= ALL       # jumps to an undecodable address (a trap)
                elif fl.falls_through and body_insn_end(e, a) not in info:
                    out |= ALL       # falls off the decoded body (undecodable next byte)
                if fl.kind == "call":
                    tgt = fl.call_target
                    if isinstance(tgt, Import):
                        uses, defs = 0, ALL
                    elif isinstance(tgt, Internal) and (tgt.seg, tgt.off) in entry_live:
                        uses, defs = entry_live[(tgt.seg, tgt.off)], ALL
                    elif fl.possible:                # indirect with known candidates
                        uses = 0
                        for p in fl.possible:
                            uses |= entry_live.get((p.seg, p.off), ALL)
                        defs = ALL
                    else:
                        uses, defs = 0, ALL          # indirect: compiled code (see above)
                new_in = uses | (out & ~defs)
                if out != live_out[a] or new_in != live_in[a]:
                    live_out[a], live_in[a] = out, new_in
                    changed = True
        return live_in, live_out

    def body_insn_end(e, a):
        return bodies[e].insns[a].end

    while True:
        new_ret = {e: 0 for e in bodies}
        changed = False
        for e in bodies:
            live_in, live_out = body_liveness(e)
            result[e] = live_out
            ein = live_in.get(e[1], 0)
            if ein != entry_live[e]:
                entry_live[e] = ein
                changed = True
            for a, (fl, *_rest) in static[e].items():
                tgt = fl.call_target
                if fl.kind == "call" and fl.falls_through and isinstance(tgt, Internal) and (tgt.seg, tgt.off) in new_ret:
                    new_ret[(tgt.seg, tgt.off)] |= live_out[a]
                if fl.kind == "call" and fl.falls_through and tgt is None:
                    for p in fl.possible:
                        if (p.seg, p.off) in new_ret:
                            new_ret[(p.seg, p.off)] |= live_out[a]
        for e in new_ret:
            if new_ret[e] != ret_live[e]:
                ret_live[e] = new_ret[e]
                changed = True
        if not changed:
            break
    violations = [(e, a, result[e][a]) for e in bodies for a, (fl, *_r) in static[e].items()
                  if fl.kind == "call" and fl.call_target is None and not fl.possible and fl.falls_through
                  and result[e][a]]
    return result, entry_live, ret_live, violations
