"""IDA analysis passes for Win16 NE binaries (the unpacked Director 4 projector, FILEIO.DLL).

Run inside IDA (e.g. via the MCP execute_python tool):
    exec(open(r"<repo>/tools/ida/analyze.py").read()); analyze(); validate()

(Don't name an entry point `run`: the MCP tool auto-invokes run() when code has no
trailing expression.)

The passes are idempotent; a second analyze() should change nothing. Segment 1 of the
projector is the OPTLOADER stub (never executed once unpacked) and is ignored.

  define_switches       MSVC `jmp cs:table[bx]` jump tables IDA missed. Some had their table
                        bytes decoded as instructions, so no case was ever decoded; one is a
                        255-case dispatch in cseg100, likely the Lingo bytecode interpreter.
  decode_targets        Every jump/switch target and fall-through successor is decoded,
                        clearing data items IDA typed too early (`dw seg X` on far-call
                        selectors, nop padding as `db 2 dup(90h)`).
  orphan_functions      Code IDA decoded but attached to no function (callbacks referenced
                        only by `offset` immediates, runtime-library fragments) becomes a
                        function wherever it can't be a fall-through from the previous item.
  prologue_functions    Unreferenced (probably dead) functions IDA never decoded, recognised
                        by MSVC prologues and checked to decode plausibly.
  call_target_functions Every call target outside a function becomes one.
  merge_*               Functions that are really pieces of another (cut off where IDA lost
                        the flow, or case blocks split off by the orphan pass) are folded back.

validate() checks the invariants the recompiler relies on.
"""
from collections import Counter

import ida_auto
import ida_bytes
import ida_funcs
import ida_nalt
import ida_segment
import ida_ua
import idaapi
import idautils
import idc

NO_FALLTHROUGH = {"ret", "retn", "retf", "jmp", "jmpf", "jmpni", "jmpfi", "iret", "hlt"}


def _is_optloader_stub(seg):
    data = ida_bytes.get_bytes(seg.start_ea, seg.end_ea - seg.start_ea) or b""
    return data[:2] == b"A0" and b"OPTLOADER" in data


def code_segments():
    """Code segments of the program proper (excluding an OPTLOADER stub)."""
    for s in idautils.Segments():
        seg = ida_segment.getseg(s)
        if ida_segment.get_segm_class(seg) == "CODE" and not _is_optloader_stub(seg):
            yield seg


_PROGRAM = None


def in_program(ea):
    """True for addresses in the program's own code (not imports, not the loader stub)."""
    global _PROGRAM
    if _PROGRAM is None:
        _PROGRAM = [(seg.start_ea, seg.end_ea) for seg in code_segments()]
    return any(start <= ea < end for start, end in _PROGRAM)


def program_functions():
    return [fea for fea in idautils.Functions() if in_program(fea)]


def where(ea):
    seg = ida_segment.getseg(ea)
    return f"{ida_segment.get_segm_name(seg)}:{ea - seg.start_ea:#06x} {ida_funcs.get_func_name(ea) or ''}"


def orphan_runs():
    """Yield (start, end) of maximal runs of code items that belong to no function."""
    for seg in code_segments():
        ea, start = seg.start_ea, None
        while ea < seg.end_ea:
            flags = ida_bytes.get_flags(ea)
            if ida_bytes.is_head(flags):
                size = ida_bytes.get_item_size(ea)
                if ida_bytes.is_code(flags) and not ida_funcs.get_func(ea):
                    if start is None:
                        start = ea
                elif start is not None:
                    yield start, ea
                    start = None
                ea += size
            else:
                ea += 1
        if start is not None:
            yield start, seg.end_ea


def can_fall_into(ea):
    """True if the item before `ea` is code that may continue into `ea`."""
    prev = idc.prev_head(ea, ida_segment.getseg(ea).start_ea)
    if prev == idc.BADADDR or not ida_bytes.is_code(ida_bytes.get_flags(prev)):
        return False
    insn = ida_ua.insn_t()
    if not ida_ua.decode_insn(insn, prev):
        return False
    return insn.get_canon_mnem() not in NO_FALLTHROUGH


def orphan_functions(max_rounds=10):
    created = []
    for _ in range(max_rounds):
        # add_func fails on code whose neighbours are still queued for analysis
        ida_auto.auto_wait()
        made = 0
        for start, end in list(orphan_runs()):
            # a new function may cover only part of a run; keep walking past it
            ea = start
            while ea < end:
                flags = ida_bytes.get_flags(ea)
                if (ida_bytes.is_code(flags) and ida_bytes.is_head(flags) and not ida_funcs.get_func(ea)
                        and not can_fall_into(ea) and ida_funcs.add_func(ea)):
                    created.append(ea)
                    made += 1
                chunk = ida_funcs.get_fchunk(ea)
                nxt = chunk.end_ea if chunk else idc.next_head(ea, end)
                ea = nxt if nxt > ea else ea + 1
        if not made:
            break
    ida_auto.auto_wait()
    return created


def _decode(ea):
    insn = ida_ua.insn_t()
    return insn if ida_ua.decode_insn(insn, ea) else None


def _match_cs_switch(jmp_ea, func):
    """Recognise MSVC's near jump-table idiom that IDA sometimes misses:

        mov ax, R / cmp ax, N / ja default / add ax, R / xchg ax, bx / jmp cs:table[bx]
    or, with an index the code already scaled by 2:
        ... / cmp ax, N / ja default / xchg ax, bx / jmp cs:table[bx]

    Returns (startea, table_ea, ncases, default_ea) or None.
    """
    jmp = _decode(jmp_ea)
    op = jmp.ops[0]
    if op.type not in (idaapi.o_mem, idaapi.o_displ) or jmp.segpref != idaapi.R_cs:
        return None
    prev = [jmp_ea]
    for _ in range(4):
        prev.append(idc.prev_head(prev[-1], func.start_ea))
    xchg, a, b, c = (_decode(e) for e in prev[1:])
    if not xchg or xchg.get_canon_mnem() != "xchg":
        return None
    doubled = a and a.get_canon_mnem() == "add" and a.ops[0].reg == 0   # add ax, R
    ja_insn, cmp_insn, cmp_ea = (b, c, prev[4]) if doubled else (a, b, prev[3])
    if not (ja_insn and cmp_insn and ja_insn.get_canon_mnem() == "ja" and cmp_insn.get_canon_mnem() == "cmp"
            and cmp_insn.ops[1].type == idaapi.o_imm):
        return None
    bound = cmp_insn.ops[1].value
    ncases = bound + 1 if doubled else bound // 2 + 1
    seg = ida_segment.getseg(jmp_ea)
    # 16-bit near operands are segment offsets, not linear addresses
    return cmp_ea, seg.start_ea + op.addr, ncases, seg.start_ea + ja_insn.ops[0].addr


def define_switches():
    """Register switch info for unrecognised `jmp cs:table[bx]` jump tables."""
    defined = []
    for fea in program_functions():
        func = ida_funcs.get_func(fea)
        for ea in list(idautils.FuncItems(fea)):
            insn = _decode(ea)
            if not insn or not insn.get_canon_mnem().startswith("jmp") or ida_nalt.get_switch_info(ea):
                continue
            m = _match_cs_switch(ea, func)
            if not m:
                continue
            startea, table, ncases, default = m
            seg = ida_segment.getseg(ea)
            targets = [seg.start_ea + ida_bytes.get_word(table + 2 * i) for i in range(ncases)]
            if not all(seg.start_ea <= t < seg.end_ea and not (table <= t < table + 2 * ncases) for t in targets):
                continue
            ida_bytes.del_items(table, ida_bytes.DELIT_SIMPLE, 2 * ncases)
            si = ida_nalt.switch_info_t()
            # SWI_USER: otherwise IDA's own (failing) idiom check deletes it on reanalysis.
            # OR it in: the constructor's SWI_VERSION bit is required for SWI_USER to count.
            si.flags |= ida_nalt.SWI_ELBASE | ida_nalt.SWI_USER
            si.set_jtable_element_size(2)
            si.ncases = ncases
            si.jumps = table
            si.startea = startea
            si.elbase = seg.start_ea
            si.lowcase = 0
            si.defjump = default
            ida_nalt.set_switch_info(ea, si)
            idaapi.create_switch_table(ea, si)
            for t in set(targets):
                other = ida_funcs.get_func(t)
                if other and other.start_ea == t and other.start_ea != func.start_ea:
                    ida_funcs.del_func(t)   # a case wrongly split off by an earlier orphan pass
                ida_ua.create_insn(t)
            ida_auto.auto_wait()
            ida_funcs.reanalyze_function(func)
            defined.append((ea, ncases))
    ida_auto.auto_wait()
    return defined


def force_insn(ea):
    """Create an instruction at ea, deleting non-code items that overlap it.

    IDA sometimes types bytes as data before the code reaching them is decoded, e.g.
    `dw seg X` items on the selector word of a far call, or nop padding typed as
    `db 2 dup(90h)`; those block code creation. Refuses to clobber existing code.
    """
    if ida_bytes.is_code(ida_bytes.get_flags(ea)) and ida_bytes.is_head(ida_bytes.get_flags(ea)):
        return True
    insn = _decode(ea)
    if not insn:
        return False
    start = ida_bytes.get_item_head(ea)
    for x in range(start, ea + insn.size):
        f = ida_bytes.get_flags(x)
        if ida_bytes.is_code(f) or ida_funcs.get_func(x) and ida_bytes.is_code(ida_bytes.get_flags(ida_bytes.get_item_head(x))):
            if x != ea:
                return False
    ida_bytes.del_items(start, ida_bytes.DELIT_SIMPLE, ea + insn.size - start)
    return bool(ida_ua.create_insn(ea))


def _falls_through(insn):
    mnem = insn.get_canon_mnem()
    if mnem in NO_FALLTHROUGH:
        return False
    if mnem.startswith("call"):
        callee = ida_funcs.get_func(insn.ops[0].addr) if insn.ops[0].type in (idaapi.o_near, idaapi.o_far) else None
        name = idc.get_name(insn.ops[0].addr) if insn.ops[0].type in (idaapi.o_near, idaapi.o_far) else ""
        if (callee and callee.flags & ida_funcs.FUNC_NORET) or name in ("FATALEXIT", "FATALAPPEXIT"):
            return False
    return True


def decode_targets(max_rounds=20):
    """Make sure every jump/switch target and every fall-through successor is decoded."""
    fixed = []
    for _ in range(max_rounds):
        want = set()
        for fea in program_functions():
            for ea in idautils.FuncItems(fea):
                si = ida_nalt.get_switch_info(ea)
                if si:
                    res = idaapi.calc_switch_cases(ea, si)
                    want.update(res.targets if res else [])
                for x in idautils.XrefsFrom(ea, 0):
                    if x.type in (idaapi.fl_JN, idaapi.fl_JF) and in_program(x.to):
                        want.add(x.to)
                insn = _decode(ea)
                if insn and _falls_through(insn):
                    want.add(ea + insn.size)
        todo = [t for t in want if not ida_bytes.is_code(ida_bytes.get_flags(t))]
        made = [t for t in todo if force_insn(t)]
        fixed += made
        ida_auto.auto_wait()
        if not made:
            break
    for fea in list(idautils.Functions()):
        ida_funcs.reanalyze_function(ida_funcs.get_func(fea))
    ida_auto.auto_wait()
    return fixed


def call_target_functions():
    """Every call target outside existing functions becomes a function (decoding it first).

    IDA occasionally types the start of a function as data, e.g. `00 b8 <sel>` (padding
    plus `mov ax, seg X`) as a far-pointer dword, so the function is never created.
    Calls into the middle of an existing function (multi-entry assembly) are left alone.
    """
    made = []
    targets = set()
    for fea in program_functions():
        for ea in idautils.FuncItems(fea):
            for x in idautils.XrefsFrom(ea, 0):
                if x.type in (idaapi.fl_CF, idaapi.fl_CN) and in_program(x.to):
                    targets.add(x.to)
    for t in sorted(targets):
        if ida_funcs.get_func(t):
            continue
        if force_insn(t) and ida_funcs.add_func(t):
            made.append(t)
    ida_auto.auto_wait()
    return made


def _only_jumped_to_from(func_start, owner_start):
    refs = list(idautils.XrefsTo(func_start))
    return refs and all(r.type in (idaapi.fl_JN, idaapi.fl_JF, idaapi.fl_F) and ida_funcs.get_func(r.frm)
                        and ida_funcs.get_func(r.frm).start_ea == owner_start for r in refs)


def merge_split_cases():
    """Fold back case blocks that an orphan pass turned into separate functions.

    A function containing a switch case target of another function, and reached only by
    jumps from that function (never called), is a misplaced piece of it.
    """
    merged = []
    for fea in program_functions():
        func = ida_funcs.get_func(fea)
        if not func:
            continue
        hit = False
        for ea in list(idautils.FuncItems(fea)):
            si = ida_nalt.get_switch_info(ea)
            res = idaapi.calc_switch_cases(ea, si) if si else None
            for t in (res.targets if res else []):
                other = ida_funcs.get_func(t)
                if other and other.start_ea != func.start_ea and (
                        other.start_ea == t or _only_jumped_to_from(other.start_ea, func.start_ea)):
                    merged.append(other.start_ea)
                    ida_funcs.del_func(other.start_ea)
                    hit = True
        if hit:
            # reanalyze_function won't grow a function's end over code past it
            _recreate(func.start_ea)
    return merged


def _recreate(start):
    """Delete and re-add a function so IDA recomputes its extent from the code flow.

    If IDA can't re-create it from flow (e.g. a branch into the middle of an instruction,
    see cseg100:0x34f4), restore the original bounds rather than losing the function.
    """
    old_end = ida_funcs.get_func(start).end_ea
    ida_funcs.del_func(start)
    ida_auto.auto_wait()
    if not ida_funcs.add_func(start):
        ida_funcs.add_func(start, old_end)
    ida_auto.auto_wait()


def _outside_refs(func):
    """Xrefs into any basic block of func that come from outside func."""
    refs = []
    for block in idaapi.FlowChart(func):
        for x in idautils.XrefsTo(block.start_ea):
            src = ida_funcs.get_func(x.frm)
            if not src or src.start_ea != func.start_ea:
                refs.append(x)
    return refs


def merge_jump_only_functions(max_rounds=10):
    """Fold unnamed functions that are only entered by jumps/fall-through from one other.

    These are pieces of a function that IDA cut off where it lost the flow; the owner
    jumps into them (often into their middle, while their own start is reached only by a
    loop inside them). Library (FLIRT) functions and anything called or referenced by
    pointer are left alone. A merge is undone if the owner doesn't absorb the code.
    """
    merged = []
    for _ in range(max_rounds):
        changed = False
        for fea in program_functions():
            f = ida_funcs.get_func(fea)
            if (not f or f.start_ea != fea or f.flags & ida_funcs.FUNC_LIB
                    or not ida_funcs.get_func_name(fea).startswith("sub_")):
                continue
            refs = _outside_refs(f)
            if not refs or any(r.type not in (idaapi.fl_JN, idaapi.fl_JF, idaapi.fl_F) for r in refs):
                continue
            owners = {ida_funcs.get_func(r.frm).start_ea if ida_funcs.get_func(r.frm) else None for r in refs}
            if len(owners) != 1 or None in owners:
                continue
            owner = owners.pop()
            end = f.end_ea
            ida_funcs.del_func(fea)
            _recreate(owner)
            absorbed = ida_funcs.get_func(fea)
            if not absorbed or absorbed.start_ea != owner:
                if absorbed is None:
                    ida_funcs.add_func(fea, end)    # owner didn't take it: put it back
                ida_auto.auto_wait()
                continue
            merged.append(fea)
            changed = True
        if not changed:
            break
    return merged


# MSVC 1.x Win16 function entry sequences (None = any byte). Unreferenced functions are
# left undecoded by IDA; we only convert bytes matching one of these to code.
PROLOGUES = [
    (0xB8, None, None, 0xC8),               # mov ax, DGROUP ; enter
    (0xB8, None, None, 0x45, 0x55, 0x8B, 0xEC),  # mov ax, DGROUP ; inc bp ; push bp ; mov bp, sp
    (0xB8, None, None, 0x55, 0x8B, 0xEC),   # mov ax, DGROUP ; push bp ; mov bp, sp
    (0x8C, 0xD0, 0xC8),                     # mov ax, ss ; enter              (smart callback)
    (0x8C, 0xD8, 0xC8),                     # mov ax, ds ; enter
    (0x8C, 0xD0, 0x45, 0x55, 0x8B, 0xEC),
    (0x8C, 0xD8, 0x45, 0x55, 0x8B, 0xEC),
    (0x1E, 0x58, 0x90, 0x45, 0x55, 0x8B, 0xEC),  # push ds ; pop ax ; nop ; inc bp ...  (exported)
    (0x45, 0x55, 0x8B, 0xEC),               # inc bp ; push bp ; mov bp, sp   (far frame)
    (0x55, 0x8B, 0xEC),                     # push bp ; mov bp, sp
    (0xC8, None, None, 0x00),               # enter N, 0
    (0x0E, 0xE8),                           # push cs ; call near             (far->near thunk)
]
PADDING = {0x00, 0x90, 0xCC}


MAX_ENTER_FRAME = 0x1000
BAD_MNEMONICS = {"hlt", "in", "out", "ins", "outs", "lock", "into", "iret", "cli", "sti", "arpl"}


def plausible_code(ea, count=8):
    """Decode `count` instructions without touching the database; reject data-looking bytes."""
    for _ in range(count):
        insn = ida_ua.insn_t()
        size = ida_ua.decode_insn(insn, ea)
        if not size:
            return False
        mnem = insn.get_canon_mnem()
        if mnem in BAD_MNEMONICS:
            return False
        if mnem == "enter" and (insn.ops[0].value >= MAX_ENTER_FRAME or insn.ops[1].value != 0):
            return False
        if mnem in ("ret", "retn", "retf", "jmp"):
            return True
        ea += size
    return True


def matches_prologue(ea):
    for pat in PROLOGUES:
        if all(b is None or ida_bytes.get_byte(ea + i) == b for i, b in enumerate(pat)):
            return plausible_code(ea)
    return False


def undecoded_regions():
    """Yield (start, end) of runs of non-code bytes outside functions in code segments."""
    for seg in code_segments():
        ea, start = seg.start_ea, None
        while ea < seg.end_ea:
            head = ida_bytes.get_item_head(ea)
            in_code = ida_bytes.is_code(ida_bytes.get_flags(head))
            if not in_code and not ida_funcs.get_func(ea):
                if start is None:
                    start = ea
            elif start is not None:
                yield start, ea
                start = None
            ea += 1
        if start is not None:
            yield start, seg.end_ea


def prologue_functions():
    """Create functions at prologue-matching addresses inside undecoded regions."""
    created = []
    for start, end in list(undecoded_regions()):
        ea = start
        while ea < end:
            if ida_bytes.get_byte(ea) in PADDING or not matches_prologue(ea) or ida_funcs.get_func(ea):
                ea += 1
                continue
            ida_bytes.del_items(ea, ida_bytes.DELIT_SIMPLE, end - ea)
            if ida_ua.create_insn(ea) and ida_funcs.add_func(ea):
                created.append(ea)
                ida_auto.auto_wait()
                chunk = ida_funcs.get_fchunk(ea)
                ea = max(chunk.end_ea, ea + 1)
            else:
                # undo whatever create_insn decoded so a false match leaves no code behind
                ida_auto.auto_wait()
                run_end = ea
                while run_end < end and ida_bytes.is_code(ida_bytes.get_flags(ida_bytes.get_item_head(run_end))) \
                        and not ida_funcs.get_func(run_end):
                    run_end += 1
                ida_bytes.del_items(ea, ida_bytes.DELIT_SIMPLE, max(run_end - ea, 1))
                ea += 1
    return created


def analyze():
    global _PROGRAM
    _PROGRAM = None
    before = len(program_functions())
    switches = define_switches()     # first, so case blocks stay inside their function
    decoded = decode_targets()
    created = orphan_functions()
    from_prologues = prologue_functions()
    created += orphan_functions()   # newly decoded functions can expose more orphans
    decoded += decode_targets()
    called = call_target_functions()
    merged = merge_split_cases()
    merged += merge_jump_only_functions()
    remaining = list(orphan_runs())
    return {
        "functions_before": before,
        "functions_after": len(program_functions()),
        "switches_defined": [(f"{idc.get_segm_name(ea)}:{ea - ida_segment.getseg(ea).start_ea:#x}", n)
                             for ea, n in switches],
        "targets_decoded": len(decoded),
        "call_targets_made_functions": len(called),
        "split_functions_merged": len(merged),
        "created_from_orphans": len(created),
        "created_from_prologues": len(from_prologues),
        "orphan_runs_left": len(remaining),
        "orphan_bytes_left": sum(e - s for s, e in remaining),
    }


def validate():
    """Check the invariants the recompiler relies on. Expected leftovers for PLAY_E:
    cross-function jumps only in the cseg02 runtime-library assembly (handled as
    multi-entry regions) and one call into a multi-entry runtime routine."""
    switch_targets, switch_problems = 0, []
    cross_jumps, odd_calls = [], set()
    for fea in program_functions():
        func = ida_funcs.get_func(fea)
        for ea in idautils.FuncItems(fea):
            si = ida_nalt.get_switch_info(ea)
            res = idaapi.calc_switch_cases(ea, si) if si else None
            for t in (res.targets if res else []):
                switch_targets += 1
                tf = ida_funcs.get_func(t)
                if not ida_bytes.is_code(ida_bytes.get_flags(t)) or not tf or tf.start_ea != func.start_ea:
                    switch_problems.append(f"{where(ea)} -> {t:#x}")
            for x in idautils.XrefsFrom(ea, 0):
                if not in_program(x.to):
                    continue
                tf = ida_funcs.get_func(x.to)
                if x.type in (idaapi.fl_JN, idaapi.fl_JF):
                    if not tf or (tf.start_ea != func.start_ea and tf.start_ea != x.to):
                        cross_jumps.append(ea)
                elif x.type in (idaapi.fl_CF, idaapi.fl_CN) and (not tf or tf.start_ea != x.to):
                    odd_calls.add(x.to)
    return {
        "functions": len(program_functions()),
        "switch_case_targets": switch_targets,
        "switch_problems": switch_problems,
        "cross_function_jumps_by_segment": dict(Counter(idc.get_segm_name(ea) for ea in cross_jumps)),
        "calls_not_to_function_start": sorted(where(t) for t in odd_calls),
        "orphan_code_bytes": sum(e - s for s, e in orphan_runs()),
    }
