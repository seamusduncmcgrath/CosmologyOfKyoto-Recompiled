"""Recompiler command line.

    .venv/Scripts/python tools/recompile.py stats build/PLAY_E.unpacked.exe build/PLAY_E.analysis.json
    .venv/Scripts/python tools/recompile.py emit build/PLAY_E.unpacked.exe build/PLAY_E.analysis.json \
        --module play_e --out generated/play_e
"""
import argparse
import sys
from collections import Counter

from recomp.module import ModuleEmitter
from recomp.program import Program


def cmd_emit(args):
    prog = Program(args.exe, args.analysis)
    result = ModuleEmitter(prog, args.module, args.out).run()
    print(f"{args.module}: {result['functions']} functions, {result['instructions']:,} instructions, "
          f"{result['imports']} imports, {result['files']} files -> {args.out}")
    for e, a, m in result["flag_violations"]:
        print(f"note: flags {m:#x} read after indirect call at {e[0]}:{a:04x} in {prog.functions[e]}")
    for err in result["errors"]:
        print(f"error: {err}")
    return 1 if result["errors"] else 0


def cmd_stats(args):
    prog = Program(args.exe, args.analysis)
    total = unique = 0
    seen = set()
    mnems = Counter()
    flows = Counter()
    invalid = []
    biggest = []
    for (seg, off), name in sorted(prog.functions.items()):
        body = prog.discover(seg, off)
        total += len(body.insns)
        for a, insn in body.insns.items():
            if (seg, a) not in seen:
                seen.add((seg, a))
                mnems[insn.mnem] += 1
                flows[body.flows[a].kind] += 1
        invalid += [(seg, a, name) for a in body.invalid]
        biggest.append((len(body.insns), name, seg, off))
    unique = len(seen)
    print(f"{prog.ne.name}: {len(prog.functions)} function entries")
    print(f"instructions: {unique:,} unique, {total:,} across all bodies (duplication x{total / unique:.3f})")
    print("largest bodies:", [(n, f"{s}:{o:04x}", c) for c, n, s, o in sorted(biggest, reverse=True)[:6]])
    print("control flow:", dict(flows))
    print(f"undecodable targets: {len(invalid)}", [f"{s}:{a:04x} in {n}" for s, a, n in invalid[:10]])
    print(f"{len(mnems)} distinct mnemonics:")
    print("  " + ", ".join(f"{m} {c}" for m, c in mnems.most_common()))


def cmd_test(args):
    from recomp import insntest
    progs = [Program(exe, an) for exe, an in zip(args.exe[::2], args.exe[1::2])]
    cases = insntest.collect_cases(progs)
    if args.filter:
        cases = [c for c in cases if c.name.startswith(args.filter)]
    dll, cases = insntest.build_dll(cases, "build/insntest", "runtime/include")
    print(f"{len(cases)} test cases x {args.vectors} random states")
    failures = insntest.run(cases, dll, vectors=args.vectors, verbose=True)
    print(f"{len(cases) - len(failures)} passed, {len(failures)} failed")
    return 1 if failures else 0


def cmd_fputest(args):
    from recomp import fputest
    progs = [Program(exe, an) for exe, an in zip(args.exe[::2], args.exe[1::2])]
    cases, failures = fputest.build_and_run(fputest.collect_fpu_cases(progs))
    for name, items in failures.items():
        for vals, probs in items[:2]:
            print(f"FAIL {name} {vals}: {probs}")
    print(f"{len(cases) - len(failures)} of {len(cases)} x87 cases passed")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("stats", help="decode everything and print statistics")
    st.add_argument("exe")
    st.add_argument("analysis")
    st.set_defaults(func=cmd_stats)
    em = sub.add_parser("emit", help="generate C for a module")
    em.add_argument("exe")
    em.add_argument("analysis")
    em.add_argument("--module", required=True, help="C identifier prefix, e.g. play_e")
    em.add_argument("--out", required=True)
    em.set_defaults(func=cmd_emit)
    te = sub.add_parser("test", help="differential-test the emitter against Unicorn")
    te.add_argument("exe", nargs="+", help="pairs of: executable analysis.json")
    te.add_argument("--vectors", type=int, default=64)
    te.add_argument("--filter", help="only cases whose name starts with this")
    te.set_defaults(func=cmd_test)
    ft = sub.add_parser("fputest", help="differential-test x87 emission and runtime FPU against Unicorn")
    ft.add_argument("exe", nargs="+", help="pairs of: executable analysis.json")
    ft.set_defaults(func=cmd_fputest)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
