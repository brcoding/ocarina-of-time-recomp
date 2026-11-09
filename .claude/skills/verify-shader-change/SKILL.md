---
name: verify-shader-change
description: Check a change to the SM86 lowering (runtime/sm86_lower.c), the shader translator (tools/xlate patches) or the shader cache before calling it done — probes, an isolated kernel on random data both ways, and screenshots of a fixed scene scored for broken output. Use after any edit that changes what a shader computes, and before reporting a speed-up.
---

# Verify a shader change

A faster frame proves nothing about the picture. On 2026-11-06 the tensor-core path ran the title at 60 fps while
drawing garbage, and a bisection ran ten tests on the emulation because its switch never applied. Do these steps in
order and stop at the first that fails.

## 1. Rebuild and re-key

- `/mnt/c/dev/metroid-prime-4-recomp/.venv-win/Scripts/python.exe tools/build_game.py`
- Translator patches changed: `.../python.exe tools/build_xlate.py` (its identity re-keys the cache).
- Lowering changed: bump `KEY_VERSION` in `runtime/gfx_shader.c`, or old translations come back from the cache.

## 2. Probes (seconds each, no game files)

    build/v131072/oot.exe --probe-tensor     # LDSM, LDSM.trans, HMMA against a CPU reference; HMMA pairs with
                                             # an accumulator chain and swapped B halves (the 16x16x16 plan), and
                                             # how the device rounds (once per HMMA, or after every step)
    build/v131072/oot.exe --probe-coopmat    # the device's cooperative-matrix shapes
    build/v131072/oot.exe --probe-vkmem      # texture layout (block-linear, GOB order), uploads

Run them through `tools/run.py`, which ends only its own game and that game's translator, since other sessions' games
may be running: `python3 tools/run.py --seconds 60 --retry 0 -- --probe-tensor` (a private build adds
`--exe DIR/oot.exe`). Never end games by image name: `taskkill /IM oot.exe` kills every session's runs. A stray one:
`python3 tools/run.py --list` shows the live instances with their pids, `taskkill.exe /F /T /PID <pid>` ends one.
`OOT_TENSOR_CORES=0` (`--env OOT_TENSOR_CORES=0`) runs a probe on the emulation for comparison.

## 3. One kernel, both ways, on data that exercises it

    python3 tools/kernel.py list --tensor                 # kernels, their HMMA counts and code hashes
    python3 tools/kernel.py xlate <id> --tensor           # lower + translate + spirv-val + op histogram
    python3 tools/kernel.py xlate <id> --tensor-mode 2    # the same for the 16x16x16 mapping (AMD; OOT_XLATE=<its translator>)
    python3 tools/kernel.py extract <id> build/logs/kern/<name>

Then `--probe-cs build/logs/kern/<name>` with a `bindings.txt` of `ssbo <slot> <hexsize> <file>` lines filled with
random halves (numpy `float16` in [-1, 1]) and `dispatch x y z`, once with `OOT_TENSOR_CORES=0` and once with it on;
compare the `out_ssbo*.bin` files. Check first that the output is not trivial: an input of zeros made one kernel
compute only its bias (12 distinct values), which proves nothing. Differences of a half-precision ulp or two are
rounding (tensor cores accumulate once, the emulation per step); differences of the size of the values are a bug.
`OOT_TENSOR_HMMA_MASK=<hex>` (with a fresh `OOT_SHADER_CACHE`) puts only the masked HMMAs of a kernel on the tensor
path, to find the one that differs. On the 16x16x16 mapping (AMD) `OOT_TENSOR_NATIVE=0` lowers one HMMA per marker
with every operand converted (the mapping before the pair plan), `1` pairs only, `2` pairs and accumulator chains,
`3` (default) also A and B straight from LDSM; an `h` suffix accumulates pairs in f16. It has cache keys of its own.

## 4. Screenshots of a fixed scene, scored

    python3 tools/run.py --seconds 30 --shot build/logs/check-on.png
    python3 tools/run.py --seconds 30 --shot build/logs/check-off.png --env OOT_TENSOR_CORES=0
    <venv>/python3 tools/shot_diff.py build/logs/check-on.png build/logs/check-off.png --out build/logs/check-side.png

A private build (`tools/build_game.py --out DIR`, its translator copied next to it) runs with `--exe DIR/oot.exe`.
`<venv>` is one with Pillow and numpy (for now `/mnt/c/dev/metroid-prime-4-recomp/.venv/bin`). A noise score
above 1.2 is not a picture (good frames score 0.4-0.6, the broken upscaler 2.1-3.1); look at the side-by-side image
too, since the two runs show slightly different moments of the cinematic. At 30 s the title shows its grass loop; the
castle and horse cinematic plays later when the title idles, and `--press-a` brings up the menu.

## 5. Something is broken: find the kernel

    <venv>/python3 tools/bisect_kernels.py            # every HMMA kernel, halved until one is left

It checks in each run's log that the subset really took the tensor path. Then go back to step 3 with that kernel.

## Reporting

Say which steps ran and what they showed, with the screenshot paths. Report a frame rate only together with a
screenshot that scored as a picture.
