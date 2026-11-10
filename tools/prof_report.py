"""Report of a sampling profile (runtime/prof_win.c, OOT_PROFILE=<path>): the samples per function, with the exe's
addresses resolved through the linker map (tools/resolve_map.py), and the hottest call chains when the profile has
them (OOT_PROFILE_CHAINS=1).

    python3 tools/prof_report.py build/v131072/oot.map <profile> [--top N]

Lines: `rip <address> <count>` (one per sampled instruction address), `stack <count> <rip> <caller> ...`,
`thread <tid> <samples> <samples in the exe>`, `base <exe base>`, `module <base> <size> <path>`.
"""
import argparse
import bisect
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from resolve_map import load_map


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('map')
    ap.add_argument('profile')
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()
    addrs, names = load_map(args.map)
    base = 0
    modules = []
    per_fn, per_thread, chains = Counter(), [], []
    total = 0
    for line in Path(args.profile).read_text(encoding='utf-8', errors='replace').splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == 'base':
            base = int(f[1], 16)
        elif f[0] == 'module':
            modules.append((int(f[1], 16), int(f[2], 16), f[3]))
        elif f[0] == 'thread':
            per_thread.append((int(f[2]), int(f[3]), f[1]))
        elif f[0] == 'rip':
            total += int(f[2])
            per_fn[resolve(int(f[1], 16), base, modules, addrs, names)] += int(f[2])
        elif f[0] == 'stack':
            chains.append((int(f[1]), [resolve(int(r, 16), base, modules, addrs, names) for r in f[2:8]]))
    print(f'{total} samples in the top addresses; threads (samples, in the exe):')
    for n, in_exe, tid in sorted(per_thread, reverse=True)[:8]:
        print(f'  {tid}: {n} ({in_exe})')
    print(f'\nhottest functions ({args.top}):')
    for name, n in per_fn.most_common(args.top):
        print(f'{n:8d} {100.0 * n / total:5.1f}%  {name}')
    if chains:
        print(f'\nhottest call chains ({min(len(chains), args.top)}):')
        for n, chain in sorted(chains, reverse=True)[:args.top]:
            print(f'{n:8d}  ' + ' < '.join(chain))


def resolve(rip, base, modules, addrs, names):
    if base and rip >= base:
        rva = rip - base
        i = bisect.bisect_right(addrs, rva) - 1
        if i >= 0 and rva - addrs[i] < 1 << 24:
            return names[i]
    for mbase, size, path in modules:
        if mbase <= rip < mbase + size:
            return f'{Path(path).name}+{rip - mbase:#x}'
    return f'{rip:#x}'


if __name__ == '__main__':
    main()
