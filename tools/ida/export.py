"""Export IDA's analysis of an NE binary as JSON for the recompiler.

Run inside IDA after tools/ida/analyze.py:
    exec(open(r"<repo>/tools/ida/export.py").read()); export(r"<repo>/build/PLAY_E.analysis.json")

Addresses are (NE segment number, offset) pairs, never IDA linear addresses. The
recompiler decodes instructions itself; this file supplies what static decoding can't
easily know: function entries and extents, switch tables, names, no-return functions,
and the indirect control flow that needs runtime dispatch.
"""
import hashlib
import json
import re

import ida_bytes
import ida_funcs
import ida_nalt
import ida_segment
import ida_ua
import idaapi
import idautils
import idc

IND_OPERANDS = (idaapi.o_reg, idaapi.o_mem, idaapi.o_phrase, idaapi.o_displ)


def _ne_segments():
    """Map IDA segments named csegNN/dsegNN to NE segment numbers."""
    segs = {}
    for s in idautils.Segments():
        seg = ida_segment.getseg(s)
        m = re.fullmatch(r"[cd]seg(\d+)", ida_segment.get_segm_name(seg))
        if m:
            segs[int(m.group(1))] = seg
    return segs


def _is_optloader_stub(seg):
    data = ida_bytes.get_bytes(seg.start_ea, seg.end_ea - seg.start_ea) or b""
    return data[:2] == b"A0" and b"OPTLOADER" in data


class AddressMap:
    def __init__(self, segs):
        self.segs = segs
        self.by_start = sorted((seg.start_ea, seg.end_ea, n) for n, seg in segs.items())

    def __call__(self, ea):
        for start, end, n in self.by_start:
            if start <= ea < end:
                return [n, ea - start]
        return None


def export(path):
    segs = _ne_segments()
    addr = AddressMap(segs)
    skip = {n for n, seg in segs.items() if _is_optloader_stub(seg)}

    def program(ea):
        loc = addr(ea)
        return loc is not None and loc[0] not in skip

    segments = [{
        "num": n,
        "name": ida_segment.get_segm_name(seg),
        "class": ida_segment.get_segm_class(seg),
        "size": seg.end_ea - seg.start_ea,
    } for n, seg in sorted(segs.items())]

    functions, switches, indirect_calls, indirect_jumps = [], [], [], []
    seen = set()   # chunks can be shared by several functions; report each site once
    for fea in idautils.Functions():
        if not program(fea):            # import thunks, OPTLOADER stub
            continue
        loc = addr(fea)
        func = ida_funcs.get_func(fea)
        chunks = [[addr(start)[1], addr(start)[1] + (end - start)] for start, end in idautils.Chunks(fea)]
        blocks = sorted(addr(b.start_ea)[1] for b in idaapi.FlowChart(func))
        functions.append({
            "seg": loc[0],
            "start": loc[1],
            "name": ida_funcs.get_func_name(fea),
            "lib": bool(func.flags & ida_funcs.FUNC_LIB),
            "noreturn": bool(func.flags & ida_funcs.FUNC_NORET),
            "chunks": chunks,
            "blocks": blocks,
        })
        for ea in idautils.FuncItems(fea):
            if ea in seen:
                continue
            seen.add(ea)
            si = ida_nalt.get_switch_info(ea)
            if si:
                res = idaapi.calc_switch_cases(ea, si)
                switches.append({
                    "seg": addr(ea)[0],
                    "jump": addr(ea)[1],
                    "table": addr(si.jumps)[1] if addr(si.jumps) else None,
                    "element_size": si.get_jtable_element_size(),
                    "default": addr(si.defjump)[1] if si.defjump != idaapi.BADADDR and addr(si.defjump) else None,
                    "cases": [[int(v) for v in res.cases[i]] for i in range(len(res.cases))] if res else [],
                    "targets": [addr(t)[1] for t in res.targets] if res else [],
                })
                continue
            insn = ida_ua.insn_t()
            if not ida_ua.decode_insn(insn, ea) or insn.ops[0].type not in IND_OPERANDS:
                continue
            mnem = insn.get_canon_mnem()
            entry = addr(ea) + [idc.generate_disasm_line(ea, idc.GENDSM_FORCE_CODE)]
            if mnem.startswith("call"):
                indirect_calls.append(entry)
            elif mnem.startswith("jmp"):
                indirect_jumps.append(entry)

    imports = []
    for i in range(ida_nalt.get_import_module_qty()):
        module = ida_nalt.get_import_module_name(i)

        def cb(ea, name, ordinal, module=module):
            callers = {x.frm for x in idautils.XrefsTo(ea) if program(x.frm)}
            imports.append({"module": module, "ordinal": ordinal, "name": name, "refs": len(callers)})
            return True
        ida_nalt.enum_import_names(i, cb)

    input_path = idaapi.get_input_file_path()
    with open(input_path, "rb") as fh:
        md5 = hashlib.md5(fh.read()).hexdigest()
    result = {
        "input": input_path,
        "input_md5": md5,
        "ignored_segments": sorted(skip),
        "segments": segments,
        "functions": sorted(functions, key=lambda f: (f["seg"], f["start"])),
        "switches": sorted(switches, key=lambda s: (s["seg"], s["jump"])),
        "indirect_calls": sorted(indirect_calls),
        "indirect_jumps": sorted(indirect_jumps),
        "imports": sorted(imports, key=lambda i: (i["module"], i["ordinal"])),
    }
    with open(path, "w") as fh:
        json.dump(result, fh, indent=1)
    return {k: len(v) for k, v in result.items() if isinstance(v, list)}
