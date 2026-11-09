# The level's picture

Hand-off for what keeps the first level (Kokiri Forest) from looking right, written 2026-11-08. It gathers the day's
measurements, the fixes, what two other projects on this game do, and the plan. Update the Status section as it
moves. The pointer design is `docs/pointers.md`, the mesh work `docs/mesh.md` and `docs/mesh-tracker.md`.

## Where it stands (2026-11-08, main at 8535fb7)

Kernel names here are input ids from before 2026-11-08's shared memory answer (`docs/phase3.md`, "compute shared
memory as the program declares it"), which changed every compute program's id: ace1cec0 is now 726b17cb, 3f15911a
7b872d67, 3ce2b98c a82c24ca, 745220f3 3a1c39ef, 1a62084a 8fb5a18e, e6d5432f 145fb221, 406ecfce 866197aa
(`OOT_SKIP_CS` and `OOT_CS_TIME` use the new ones; mesh and task programs keep theirs).

On the level route (`python3 tools/run.py --seconds 150 --replay build/logs/a-spam.txt --skip-movies`, no other
settings) the game reaches the level, runs at about 59-60 fps there, and draws its geometry: the wooden structures,
bark and moss, the ground, Navi. 0 validation errors, 0 shader failures in the boot summary (but see "Translator
failures" below). What is wrong:

1. **White areas.** Large regions are blown out white (`build/logs/fix4-level.png`, `merge-level-side.png`).
2. **Stretched triangles.** Long white slivers and spikes cross the view.
3. **Two kernels skipped by default** (`vk_exec_draw.c skip_cs`, `SKIP_CS_DEFAULT`, from #12's ee54468):
   `ace1cec0` (the level's background, 64x64 groups, six uniform slots: black and white blocks when it runs) and
   `3f15911a` (bit masks, see below). `OOT_SKIP_CS=none` runs them; every skip is logged once.
4. **Some mesh records still zero.** The shadow pass's (1600x1600) indirect mesh records read 0 0 0 0 in frame dumps
   455 and 456, so part of the culling chain still counts nothing (`docs/mesh-tracker.md`, log of 2026-11-08).

Before #12 the level was all black (the mesh draws drew nothing, `docs/mesh-tracker.md`).

## What was measured

**The white is unmapped HDR.** The end-of-frame kernel `3ce2b98c` (200 instructions, 128x1x1, writes R11G11B10F
through an R32UI view, 120x68 groups) works in place on the scene's HDR target. In frame dump 456 (frame 4500,
`3f15911a` skipped) its output equals the HDR buffer of pass 144 almost exactly (same minimum, max 34.5, 413,533
against 413,532 distinct values): it changed almost nothing, and values above 1 show as white. In dump 455
(`3f15911a` running, `OOT_SKIP_CS=ace1cec0`) its output still reaches 35.5. So `3ce2b98c` processes only what a mask
selects, and the mask it reads is nearly empty.

**The mask.** `3f15911a` (1304 instructions, 96x1x1, indirect dispatch between the histogram kernel `745220f3` and
`3ce2b98c`) builds bit masks (FLO, POPC, BREV) and ORs them with SURED into an RGBA8 image (tex 295, 1920x1080, at
0x1502ee0e00) that it views as R32UI (`nvnTextureViewSetFormat`, NVN format 0xB); `3ce2b98c` samples the same
address. Its output in dump 455 is almost all zero, a few speckles along the top edge and at the top right. It was
skipped (it did not lower) until main's LEA with a negated addend let it through, which is when the flickering
white patches appeared (#12, ee54468). Checked, all correct for the words in this kernel:
- the only negated LEA, at 0x4b50: `LEA.HI R1, R1, -R0, RZ, 30`, that is (R1 >> 2) - R0 (Lichii, Fritchii and
  Hookshot read bit 63 as negating Rb too, and lower it the same way);
- the FLO (unsigned, no .SH), the IADD3 31 - x, the SHF.R.U32.HI around it;
- the three SURED (0x59f): 2D, U32, OR, handle c[0][0x210];
- the translation: no dropped global access (empty notes), both LD resolve to storage buffers (c0+0x130, c0+0x1c0),
  the image is R32UI with an atomic OR;
- the binding: `reinterpret_format` should give the R32UI view (none of its exclusions applies to tex 295).
So the lowering of `3f15911a` is not where the bug is; its inputs, or what feeds the indirect dispatch, are next.

**Translator failures since #12.** 45 compute programs fail in the translator with "InvalidOperationException:
Invalid storage kind GlobalMemory" (runs 455, 456; none before the merge): a global access reached the SPIR-V
back end unreplaced. Their dispatches are skipped ("its shader did not translate"). The boot summary's "shader
failures 0" does not count them. Since 8535fb7 the cache keeps each refused program (`<key>.xfail.code`,
`<key>.xfail.json`) and the log names the stage; one reproduces offline (`oot_xlate` on the job with its `code`
pointed at the .xfail.code). Suspect: the GlobalToStorage changes of patches 0022/0023 (#12). Not yet found.

**A crash that was memory.** Twice `FATAL: rt_dispatch: no function at guest pc main+0x1661690` near frame 2830, with
"the system 84718 of 85781 MB" committed: another emulator (Lichii) was open. With it closed the same route ran 200
s. The game commits about 20 GB on the level; leave room.

## Fixed today

- **F2IP** (112df06, GuiFlam's from #7): 0x243 with a destination register is F2IP.U8.F32 (two floats clamped to
  bytes); it was lowered as FCHK, which dropped the result and cleared P0 (2 compute and 2 mesh programs of the cache).
- **HMNMX2** (cb3eed5): bits 73 and 62 are |a| and |b|, as every other half-precision decoder of ours reads that field
  and as Lichii, Fritchii, Hookshot and Mesa NAK do; the merge of #12 had put back the reading as negations. The
  histogram kernel `745220f3` uses them 31 times.
- **IMAD** (9e0fed1): the addend's negate is bit 63 in forms 3 and 7 (constant or uniform register in the wide slot),
  not 75 (`-UR4` became `+UR4`; 6 instances). The census of 14,363 programs refuses none for it.
- **SHF** (1eeae88): Ryujinx's MaxShift is 0 for 32-bit and 2 for U64; we wrote them swapped (wrong for shifts >= 32).
Probes pass, the title is unchanged (noise 0.55), the level looks the same: these were real, not the main cause.

## What the other projects on this game do

Both are GPL-3.0: read for behaviour only, no code taken. Local checkouts: see `CLAUDE.local.md`.

**Fritchii** (Frizti/Fritchii, a Lichii fork; commit c9ebb5c3 is its work on this game): instruction fixes (the
four above came from comparing with it), arbitration type 3 for WaitIfEqual64, compute storage bindings sized
min(layout, 8 MiB). It skips no program of this game by default (`EDEN_NX2_REJECT_HASH` is opt-in).

**Hookshot** (NotRexed/Hookshot, a native recompilation of this game; GPL-3.0 since 1720815, 2026-11-07, MIT before:
`vk_exec_draw.c` cites its `gpu/vk/vk_bank.cpp` for the task/mesh bank 0 and the compute uniform table, behaviour only).
It runs `3ce2b98c`, `3f15911a` and `ace1cec0` unmodified (its keys for them: 6637ac1d052d22d4, 685adb41edc19de7,
6f2a263dfafb02fe; FNV-1a-64 over the control's first 0xd8 bytes, then the code up to control+0x44,
`gpu/vk/vk_shader.cpp code_key`); `3ce2b98c`'s output averages about 0.7 there. What it does that we do not:
- **A pointer-pool table at c0+0x880** for task, mesh and compute stages (`vk_bank.cpp pointer_table`): the host scans
  the stage's small bound storage buffers (up to 256 bytes) for words that are GPU addresses outside every binding
  and binds up to three whole pools there. Its note: without it "skinned and instanced models lost their bones and
  collapsed into spikes". Ours: `vk_c0_fill` returns early for task and mesh stages; #12's `ptr_targets_add` binds
  pointer targets into free storage slots for vs, fs and cs only.
- **The mesh stage buffer zeroed** at the start of a mesh program (ae4fb30): the epilogue copies every vertex slot
  and index byte below the primitive count, so bytes never written were leftovers, "hundreds of long thin
  triangles". Our patch 0020 does not zero it.
- **Bare pointers bound to the end of their pool** (ffcdb71, 3858629): a table entry whose size word is 0; the fog
  reads light probes up to ~130 MB past such a pointer. A read-only bare entry is bound to the end of its pool (up
  to 1 GB), a written one with 1 MB, of which only the first 64 bytes are written back (copying back the guessed
  1 MB overwrote the CPU's data). Reads past the binding gave 0 and the fog went overbright: white haze.
- **Exposure meter coherence** (98563eb, e84d467, 8c28ed7): a GPU pass sums the frame's brightness into a small buffer
  that the CPU reads and resets; the CPU's read waits on the work that writes it, and pages the CPU reads back are
  copied again from guest memory at the start of each submission. Wrong, the exposure stays wrong.
- **Finite colour outputs** (`docs/design.md` section 10, `GuardFragmentOutputs`): one NaN spreads through bloom into a
  white frame.
- **Pointer page table**: flat, 4 KiB pages over two windows, PhysicalStorageBuffer reads, its address in a push
  constant; stores through it off by default (they faulted a GPU), task and mesh behind a switch. Close to our P1/P2.
- Smaller: HMNMX2's immediate form 0x840 (we refuse it: fragment program 7132f633); signed and SHF.R.HI funnel
  shifts (we refuse them); point topology for mesh programs (control 0x5f = 1; all 42 of our mesh stages are 3,
  triangles); 8- and 16-bit global loads in the fallback search; storage tables for tessellation and geometry
  stages; point size 2047.9375 (ours 2047.0). It skips draws of one program (53509044401d78d2, not in our cache)
  and replaces the TAAU network with a linear upscale only where matrix multiplies are emulated.

## Plan

1. **The pointer-pool table at c0+0x880 for task, mesh and compute** (the spikes). What each stage's program reads
   there, how many entries, their layout; then the host side, reusing #12's pointer scan (`ptr_targets_add`).
2. **Zero the mesh stage buffer** at the start of a mesh program (translator patch 0020 or a new one).
3. **Bare pointers bound to the end of their pool, and the exposure readback** (the white).
4. **The 45 translator failures** ("Invalid storage kind GlobalMemory").
5. **P2** of `docs/pointers.md`, the page-table fallback (my edits saved before #12, to be rebuilt on its 0022/0023).
After each change that alters what a shader computes: the `verify-shader-change` skill.

## Open questions and loose ends

- `3f15911a`'s inputs: its indirect dispatch's group count (which kernel writes it), tex 14608 (rg16ui 480x270) and
  tex 292 (rg8 192x128) that it reads.
- HMNMX2 is lowered as `m * (a - b) + b`, which gives NaN when an operand is infinite or |a - b| overflows half
  precision; Fritchii widens to f32 and uses FMNMX. The same in `h2_relu` with -inf. A NaN source for white areas.
- ISETP.EX restores its saved predicate under the instruction's own guard; Fritchii unconditionally (no program of
  the cache is affected).
- Ryujinx's funnel shift `(A >> B) | (C << (32 - B))` shifts by 32 when B is 0, which SPIR-V leaves undefined; NVIDIA
  gives the right answer, other GPUs may not (#9 is an AMD card). Answered 2026-11-09: `oot.exe --probe-shf` runs every
  form lower_shf passes on (SHF.R and SHF.L.HI, U32 and U64, wrap and clamp) with amounts 0-219 against a CPU
  reference. The RX 9070 XT returns A | C for every amount that wraps to 0 (14 of 768 results), and Ryujinx gives 0 for
  the clamped 32-bit forms at amounts of 32 or more, where the hardware clamps the amount and keeps the other word (128
  more, on any GPU). The level reaches the first: the programs it loads (`shader-programs.bin` of a level run) hold 20
  mesh and task programs with 726 true funnel shifts, all SHF.R.U64 without .W, 610 of them by a register (the census
  corpus has only 9 of the level's mesh programs and none of these). Ryzen1998's fork found the same from the other
  end (c7b8e4b, read for comparison, no code taken: meshlet indices at bit offsets that are multiples of 32 picked
  unwritten vertices, stray triangles) and fixed it in the lowering. Translator patch 0029 fixes both cases at their
  source, Ryujinx's EmitShf; the probe passes with it.
- RED.ADD.F32 offsets are cut to 20 bits silently (Fritchii range-checks them).
- `tools/xlate/wip/0025` (#12, numbered 0024 there before main took 0024) was written for the branch's page-table layout, which the merge left out; it does not
  match P1's table at support-buffer byte 1408.

## Status

- 2026-11-08: written; next is item 1 of the plan.
- 2026-11-08: item 1, as it turned out. No program of the game reads bank 0 at 0x880 or above (a scan of the
  14,363 census programs: the highest offsets are the task and mesh tables at 0x670 and 0x770): Hookshot's table there
  is its own, an area its fallback searches. Ours is #12's: 0022 searches the stage's storage table, ptr_targets_add
  fills its free slots. What was missing, now done:
  - translator patch 0024: 32 storage-buffer slots instead of 16 (0022's sixteen table targets crowded out the
    shader's own buffers: "Failed to reserve storage buffer", 624 dropped loads and 12 atomics in 33 compute programs,
    72 loads and 12 atomics in 9 mesh programs, 6 loads in 3 task programs; and the 45 "Invalid storage kind
    GlobalMemory" failures, all 86 refused jobs kept so far now translate and pass spirv-val), and the task and mesh
    tables (0x670, 0x770) for 0022's search, where those stages searched compute's 0x310;
  - `op_draw_mesh_ptr`: pointer targets bound for the task, mesh and fragment stages of mesh draws (run 463 logs them,
    e.g. a 64 MiB target for mesh storage slot 1).
  Level route: 0 InvalidOperation failures (19 lowering refusals left), ~54-59 fps; the picture still has the white
  areas and the spikes (`build/logs/item1-level.png`, a different moment of the route), the title is unchanged
  (noise 0.54), and the chain's indirect records still read 0 from frame 4000 (`OOT_MESH_IND_LOG=4000`). Next: item 2.
- 2026-11-08, later: what the white is and where the culling counts go.
  - The white is mostly missing geometry: the end-of-frame chain (pass 161 scene, 162 histogram `745220f3`, 163
    `3ce2b98c`, 164 the TAAU upscaler `406ecfce`, 165 the tone map to the screen) already has the white shapes in
    pass 163's input; the upscaler multiplies those values about 160-fold (max 16,896). Running `3f15911a` or
    everything (`OOT_SKIP_CS=none`) does not change it. #13 (occlusion reports kept per address) brought much of the
    missing geometry back.
  - The culling chain's indirect records are written by the game's CPU code each frame (`main+0x2bfd340`, called
    from `main+0x566fd0`, found with `OOT_WATCH_WRITE`), after it reads pass 0's output (`0x29bb9c000`, by
    `main+0x2bfd750`); nothing on the GPU writes them by binding, and no copy or clear does. `OOT_WATCH_POLL` in
    `OOT_MEMORY=import` saw pass 0's output and the records never change over 3,500 frames.
  - Pass 0 (`e6d5432f`, 1,952 instructions, no textures) counts with six RED (ADD and MAX, U32) through pointers it
    loads from its bank 4 (uniform slot 1, `LDC c[4][Rn]`): a table of pointers to the records themselves. None was
    bound, so 0022's search dropped every count; 93dd87b binds what uniform buffers point at too. The records still
    read 0 with it, in both memory modes, so either the RED addresses fall outside the bound spans or the kernel does
    not reach them (its own stores to 0x29bb9c000 never land either). Its P2R, compares and selects are next; the
    probe (`OOT_PROBE`) writes images only, and this kernel writes buffers.
  - The game embeds Lua; its error longjmp crashed us at `main+0x1661690` (d105aa9 fixes it, runtime/sjlj.c).
  - New dump (`files/`, base + update packages, 186,583 RomFS files as other players have) in use since this
    evening; code identical, 9 RomFS files differ from the old one, none in Kokiri Forest.
- 2026-11-08, night: pass 0's path, measured with the buffer probe (tools/probe_reach.py, OOT_PROBE ...:b<slot>, slot 6
  `0x2a3711000`, which the CPU does not read, holds all 21,632 threads).
  - The kernel gives each warp a category from a prefix-sum table in its bank 3 (c[3][0x660..0x6f0]: 0, 0, 141, 286,
    360, 360, 614, 652, 675, 675 warps; read correctly) and then each category's item count through two levels of
    pointers: bank 4 (uniform slot 1) -> a table at 0x29bb2b880 -> the category's buffer (0x2a19e9000 for 2,
    0x2a1b4c000 for 3, ...), whose first word is the count. A warp past its count exits at 0x430.
  - Category 2 read its count (4502) and worked; category 3 read 0 although the memory held 4618, because its buffer
    had no slot: the run-time table search (0022) has 16 entries and the kernel's own buffers plus the pointers in
    its storage and uniform buffers are more than that. Categories 3-8 (79% of the threads) exited at once.
  - A 32-entry search for compute (my patch 0025-compute-table-32-entries, 7bbdb65) made category 3 read 4618 and
    rendered well under OOT_MEMORY=import but black with VRAM mirrors; #16's slot sharing (below) fixes the same loss
    in mirror mode, so the merge of #16 dropped the 32-entry patch and its host side.
- 2026-11-08, night (RTX 2080, mirror mode): pass 0's counts reach their records. `OOT_PTR_TRACE=e6d5432f,<frame>`
  (new, `vk_exec_draw.c ptr_trace`) showed the kernel runs (its warp ranges at `c[3][0x660..0x6f0]` read 0x8d to
  0x2a3) and that what it lost was slots: the game binds 12 storage slots, bank 4 points into eight pools, and only
  the four left free took a pointer target, so the counts through the others were dropped. Fixes, all host side:
  - for an NVN2 kernel with no slot left, a game slot the kernel reaches through its own bank-0 table entry (traced)
    gives its run-time search slot to a pointer target; the NVN2 tables and the traced bindings keep the game's
    bindings (`game_ssbo`, `nvn2_traced_slots`). All nine targets bind now;
  - a written pointer target writes back its first 256 bytes in mirror mode (before, targets were bound read-only and
    nothing counted through them reached guest memory); without `vk_usnap_drop`, which invalidates every cached
    resource set (1 fps with one per dispatch);
  - the pointer scan reads a block's snapshot (vk_usnap.c), and scans uniform blocks for compute, task and mesh stages
    only: 93dd87b's scan of vertex and fragment uniform blocks took the level from 28 to 1 fps here
    (`OOT_PTR_BIND_GFX=0` gave 28 back).
  Picture: the big white wedges and the sky showing through are gone (`build/logs/ptrwb-level3.png` against
  `ptrwb-level.png`, same moment; `ptrwb-level4.png`, Navi's scene, one white streak left). Left: the stretched
  triangles (plan item 2) and a white patch on the water. Import mode runs at 17 fps on this machine: use mirror mode.
- 2026-11-08, later still (RTX 2080):
  - Frame rate: `ptr_targets_add` took 71% of the render thread (`OOT_PROFILE_RENDER`); `ptr_scan` caches each
    block's pointers per submission: 16.8 -> 27-28 fps on the level, now GPU-bound near 31 ms a frame, of which the
    histogram kernel `745220f3` takes 12.2 ms per dispatch (`OOT_CS_TIME=1`): the next target for 60 fps.
  - Plan item 2 done: translator patch 0025 starts task and mesh programs from a cleared count (a culled meshlet exits
    before writing it, and the epilogue emitted the previous workgroup's leftovers): no more white spikes
    (`build/logs/mesh0-level.png`). Patch 0026: local memory starts at zero (Hookshot's 08b413e).
  - `ace1cec0` runs by default again: no blocks, no black scene on the route (`build/logs/ace-on-level.png`).
  - The exposure readback wait (`vk_backend.c guest_read`, after Hookshot's 98563eb) turned the picture black; it is
    off by default (`OOT_READBACK_WAIT=1`). `OOT_READBACK_LOG=<n>,<frame>` logs what the CPU reads back: about 8
    buffers a frame, among them 0x1040-byte ones written by 1x1x1 kernels 43cc74f6 and 90e682b1 (-10.0 every fourth
    word from word 5). Open: the overexposure (white sky, posterized colours) and the missing grass.
  - The white tangle (with its own shadow) over the Deku Tree's first scene and on the water is the grass: mesh
    program `bd5a7c84` (task `0539c559`), found by bisecting the 34 mesh programs of the scene (`OOT_MESH_SKIP`,
    `OOT_MESH_LIST`) on the player's recorded route (`run.py --record`, `build/logs/blob.txt`). It loads from its
    storage slot 0 at negative offsets (`[UR4 - 4]`, `[UR4 - 16]`, UR4 = c0 0x770), which the foliage programs that
    draw right do not. Skipped by default (`MESH_SKIP_DEFAULT`) until that is fixed: no tangle, no grass. Clearing the
    whole mesh stage buffer at the start (tried as a patch 0027) did not change it.
- 2026-11-08, night (RTX 2080): the black frames while walking (whole frames black but the HUD, 4 of 17 shots of the
  recorded walk `build/logs/walk.txt` at 44-60 s, none with `OOT_GPU_AHEAD=0`). Cause and fix:
  - `OOT_DUMP_TRACE=<lo>-<hi>` (new, `vk_dump.c`): the centre and corner of every large target, read back at its
    fence with no wait, so the CPU and GPU keep their overlap (a dump, a screenshot and `OOT_GPU_AHEAD=0` all slow the
    frame down, and none of them ever showed a black frame). On a black frame the first bad value is a NaN in Link's
    own colour target (`0x1505bd1000`, his material pass, fs `0eabd88d`); the passes after it (his downsample, the
    scene, bloom, `1cb856c3`, the upscaler `eb41de8d`) spread it over the whole picture and the tone map makes it
    black. `OOT_DUMP_TRACE_DRAWS=a<addr>` samples after every draw of that target: the NaN comes from Link's draw 0,
    every other frame, with his uniform blocks (NaN-free apart from integer masks in block 9, as on clean frames) and
    vertex buffer (unchanged between record and completion) as the game wrote them.
  - It depends on the frame rate, not on a race: at a fixed 30 fps there are none (0 of 53 shots), at about 38 fps the
    frames alternate between one and two vsync ticks and the NaN comes on the alternate frames. Hookshot sees the same
    (its "dark frames": 5% at speed, none at 33 fps; read for behaviour only).
  - Fix: the pacer holds whole rates (`nvn_api_obj.c`, adaptive interval): 30 fps while the work misses 60 Hz frames
    (8 in a row over 18 ms), 60 again when it averages under 13 ms for 180 frames. Walk: 0 black of 53 shots, the level
    at 30 fps (it was about 38, uneven); the title still runs at 60. `OOT_PRESENT_INTERVAL=0` turns it off, `=<n>` paces
    at n ticks always.
  - Ruled out on the way: occlusion conditions (`OOT_COND_RENDER=1` turns the picture white; waiting for every report
    of an earlier submission waited once a run), the exposure meter `0x25a4c7f00` (reads 1/1024 every frame), mirror
    write-back (import mode has them too), read-back pages refreshed into the mirror (Hookshot's e84d467; no change),
    the uniform snapshot (`OOT_UBO_SNAPSHOT=0`: fewer NaN frames, not none), Ryujinx's ignored `.FMZ` (our lowering
    refuses SM86 `.DNZ` FFMA and FMUL, so no lowered fp32 multiply needs it).
  - Left: the white spikes from Link while he moves (stretched triangles), at 30 fps too.
- 2026-11-09 (RX 9070 XT): that first pacer rule left the black frames on this GPU. On the level route (warm caches,
  `run.py --alone`, `OOT_SHOTS=w120-200:2`) about half the shots were black (19 of 41 on main 17b8c4b, run-82; 22 of
  51 with the shared-memory commit), at an uneven 46-49 fps. It went to 30 only after 8 frames in a row over 18 ms of
  CPU work and back once that work averaged under 13 ms. At 60 the level's frames take 14-38 ms, about 20 on average
  (perf line: GPU 12.3 ms, and other threads wait 9.6 ms of each frame for GPU fences, the game's own once-a-frame
  sync), but seldom 8 in a row over 18 ms of work; at 30 the work is 10-12 ms, so it went back to 60 every few seconds
  and stayed there. The title without the shared-memory commit flipped the same way (CPU work 2 ms, GPU 18.3 ms).
  - New rule (`nvn_api_obj.c`, adaptive interval), over a sliding window of 120 frames. At 60, a frame's cost is the
    time since the previous present less the pacer's own sleep in the acquire (GPU fence waits count); 12 of the
    window over 17.5 ms switch to 30. At 30 the pacer predicts the cost at 60 as CPU work (less every acquire wait)
    plus GPU time (the timestamps of `OOT_TRACE=perf`, now also taken for the pacer, averaged over 8 presents), which
    assumes no overlap, as the fence waits above show; back to 60 when a full window has at most 6 frames predicted
    over 16 ms and 30 was held 5 s. A return that misses again within 10 s doubles the next hold (up to 320 s).
    `OOT_TRACE=perf` logs both measures every 600 presents, in every mode (`nvn: pacing at <n> ticks: ...`, with
    medians and percentiles).
  - Level, main 17b8c4b + the rule (run-88): 0 black of 41 shots, a steady 30.0 fps (1% low 27.8) from the level's
    start to the end; at 30 every frame is predicted over the line (CPU work 9.8-10.2 + GPU 12.6 ms on average). The
    only switches are in the start-up (to 30 while loading, back on the title, to 30 again within 10 s: hold 10 s).
  - Title, with the shared-memory commit (run-90): to 30 while loading, back to 60 after 6 s (CPU work 3.7 + GPU
    10.4 ms predicted at 30; the GPU reads about 10% slower at 30 than at 60), then 60 to the end: 59.2 fps over the
    second minute (1% low 56.3; six frames of about 160 ms where the title loop hands over to the cinematic, under the
    12 that switch). At 60: CPU work 2.2 + GPU 9.4 ms, at most 1% of frames predicted over 15 ms.
  - Title, main 17b8c4b + the rule (run-92): 30 fps (29.9 over the second minute, 1% low 29.4; predicted median
    20.4 ms, 5th percentile 20.0). One return to 60, where the title loop hands over to the cinematic (4 s of light
    frames, GPU 1.4 ms): it missed again after 35 frames and held 30 from then on, the hold doubled to 10 s. The first
    rule flipped this title every few seconds (run-65).
  - Level, with the shared-memory commit (run-94): 0 black of 41 shots, 30.0 fps from the level's start to the end
    (1% low 26.5-27.4; predicted CPU work 10.1-10.6 + GPU 11.5 ms, 5th percentile 19.8 ms). Runs 88-94 were taken with
    `--trace perf` (as run-82), whose line every 600 presents is where the numbers above come from; run-88 had the
    window's first version (fit line 15 ms), the others the committed one.
- 2026-11-09, night (RX 9070 XT, 16 GiB, Windows): a level run on a 16 GiB card ended with exit 0xC0000409 twice
  (runs 9 and 123). It is the AMD driver aborting (amdvlk64.dll 32.0.31041.1004 +0x2f3d901, int 29h, fast-fail 7:
  `abort()`) inside `vkAllocateDescriptorSets`, called from `alloc_set` (`vk_exec_draw.c`, a draw's `bind_resources`),
  read from the WER minidumps in `%LOCALAPPDATA%\CrashDumps` with cdbX64. Why: once Link walks outdoors, the level's
  textures, ASTC decoded to RGBA8, outgrow the 13.2 GiB VRAM budget; `vmaCreateImage` (`AUTO_PREFER_DEVICE`) then puts
  textures in system memory, and `vkCreateImage` fails with -2 for some. The process's commit rose from 23 GiB indoors
  to 27-32 GiB in every outdoor walk (nine runs; run-123: +4.3 GiB in the first 25 s outdoors, 30.8 GiB at the abort),
  toward the system's commit limit (39.5 GiB on that machine: 31 GiB of RAM and an 8.6 GiB page file, shared with the
  other programs), where the driver met an allocation it could not make. The GPU paged as well: 20-28 ms a frame
  outdoors. The 12 GiB line of e609361 put 12 GB
  cards such as the RTX 4070 on BC3, but not 16 GiB ones.
  - Fix: ASTC is encoded to BC3 by default below 20 GiB of VRAM (`vk_astc.c`). The same 7-minute walk, build and pacer
    (#19's) with BC3 (run-130 against run-123): commit flat at 26.3 GiB, the GPU at 15.5-16.5 ms a frame outdoors
    instead of 20-28, a steady 30 fps (1% low 26-27 instead of 15-16), and 3.1% of the presents black (`OOT_DUMP_TRACE`,
    last target all zero) instead of 27.4%. Title at 30 s with and without: both pictures (noise 0.46 and 0.47).
- 2026-11-09, late (RTX 4070): a guest crash leaving Link's house (the player's recorded walk
  `build/logs/grass-route.txt`, run-585, about 100 s in; exit 71). Not reproduced: 0 of 7 replays of the same script
  (runs 593-604; the script is timed by the world clock, so a replay drifts from the session). What it is:
  - `main+0xc938d0` (from `main+0x560b38`, per-record geometry work) takes a record (x0, here `0x29bc11000`): a
    position, a rotation, an object id at +0x1c (0xbf) and a flag at +0x20. It looks the id up in a table
    (`[[0x7004e22540]+0x120]`: count at +0x840, pointers at +0x848) and reads +0x150 of the entry without a null
    check; the entry was null, so it read address 0x150. The game trusts the id.
  - The records are written each frame by the CPU job `main+0x2bfd750` (via `main+0x566fd0`, a different worker
    thread each frame): it sets id -1 and flag 1 first, then copies the id from +0x4 of 0x30-byte entries of pass 0's
    output, `0x29bb9c000` (the culling kernel 145fb221, its storage slot 10, 0x1d840 bytes), at offsets of 0x15fa0
    and more (`OOT_WATCH_WRITE=0x29bc1101c:4 OOT_WATCH_REGS=1`: r8 held the buffer, rax the offset).
  - Slot 10 is one of the game's own bindings, marked written by the translator (flags 1), so the whole range is
    written back when the dispatch completes: the 256-byte write-back of pointer targets (above) does not apply. The
    game reads it directly, with no pool invalidate, so `OOT_READBACK_LOG` (guest_read) never sees it.
  - Suspect, not shown: at the area change the CPU job reads an instance list made for the previous area (or from its
    inputs), whose ids the new area's table no longer holds. Hookshot met the same class of bug from the exposure
    meter (ea78039, e84d467, read for behaviour only): a mirror copy taken at the wrong moment puts one side's older
    words over the other's newer ones on pages both the CPU and GPU write. Next: a check at `main+0xc938d0`'s entry
    counting records whose id has no entry, mirror mode against `OOT_MEMORY=import`, on a route that crosses areas.
  - New diagnostic: `OOT_WATCH_REGS=1` adds the host registers to every `OOT_WATCH_WRITE` trap (watch.c).
  - The replays also showed what the walk looks like: black frames with white blotches (NaN frames) indoors and out,
    Link's white spikes, the overexposed canopy outdoors, and stalls of 250-320 ms outdoors with about 15 ms of replay
    in them (the area streaming in, not rendering).
