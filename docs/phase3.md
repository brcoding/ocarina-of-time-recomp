# Phase 3: SM86 shaders

Hand-off written 2026-11-05 at commit `b46d663`, for a fresh session. Read `CLAUDE.md` and `AGENTS.md` first; the
history is in `docs/plan.md` (Phases 0 to 2 and the NVN2 survey). This file says where the port stands, exactly what
blocks it, what is already measured, and the order to do the work in.

## Status (2026-11-05, later the same day)

Steps 1 to 3 of the order of work below are done; step 4 is at 86% of the corpus; step 5 is where the work is.

- The translator builds (`build/xlate/oot_xlate.exe`, `python tools/build_xlate.py` from the Windows venv, 20 s).
  `tools/xlate/Program.cs` now echoes the job's `c1` into the `.res.json` (the host binds the lowered jump tables).
- `tools/shader_census.py` (NVN2 version; `python3.14`, Linux) reads the archives and writes
  `build/v131072/shaders/{ctl,code}/<key>`, `programs.json` and `opcodes.txt`: 14,363 programs (10,994 pixel, 3,106
  vertex, 251 compute, 9 mesh, 3 task) from the four `Shader/*.bfsha.zs`, the `Layout2Shader` bfsha and the two agl
  `.sharcb` of the ApplicationPackage sarc; the ExeFS modules hold none. `--lower` builds `runtime/sm86_lower.c` with
  gcc (`tools/sm86_lower_main.c`) and lowers every program, grouping the failures with the failing instruction in
  `lowered.txt`; `--translate` runs the translator over the lowered ones through WSL interop (`failures.txt`).
  `tools/sm86_dis.py <key>` lists a program's fields. The corpus is regenerated from the RomFS in 2 min; later runs
  reuse `programs.json` (`--extract` reads the archives again).
- The control section's fields are settled (the table below, columns "Reading", and `sm86_control_parse`): size at
  `+0xd4` (the BNSH archives repeat it at `+0x30` and `+0x98`, the agl packages leave those 0), instruction end at
  `+0x38`, jump-table constants at `+0x40` with their length at `+0x3c`, code size at `+0x44`, the stage at byte
  `+0x56` (NVN's enum plus 6 task, 7 mesh), the compute workgroup size at `+0x58` and its shared memory in bytes at
  `+0x64` (2026-11-08, below); the tail past `+0xe0` holds a u32
  count and 16-byte records `{BRX instruction offset, entry count, 0x10000, byte offset of the table in c1}`.
  Graphics code starts with the 0x80-byte Turing+ header, whose first 0x50 bytes are the Maxwell header the
  translator reads. The early-fragment-tests flag is not identified (candidates: `+0xa4` bits 0 and 8).
- `runtime/sm86_lower.c` lowers 13,444 of 14,363 programs (93.6%). Registers: the lowering's uniform registers
  and scratch live above the program's own; a uniform register loaded by a single direct `ULDC` (or `UMOV`), or by
  one earlier in the same basic block, is forwarded as that constant and needs no register; when the program leaves
  no room, a liveness analysis over the basic blocks (`insn_regs`, exact destination widths, reads over-stated only
  for texture coordinates) finds scratch among the registers dead at each instruction and a uniform register's
  register among those dead over its span. Uniform predicates take free predicates, or are swapped through a
  register around each instruction naming them. What is left: 656 vertex shaders (skinning: up to 247 registers
  live at once, no adjacent pair free where a load needs one), the surface stores (`SUST`, 86 compute programs),
  `ISBERD`/`ISBEWR` (task and mesh stages), a few rare forms (`lowered.txt`).
- The host (`runtime/gfx_shader.c`, `vk_exec_draw.c`, `vk_dump.c`, the shader probe and the pipeline self-test)
  reads the NVN2 control through `sm86_control_parse`, hashes the control section (its own size) and the whole code
  block for the cache key (`KEY_VERSION` 5, `IN_VERSION` 2, `PRG_VERSION` 2), lowers before translating, and binds
  the lowered program's c1 (`gfx_shader_c1`, uploaded once per shader) as uniform slot 1 instead of the game's, since
  the jump tables' targets moved with the code.

## Status (2026-11-06)

The game plays its title cinematic through a host decoder, and the picture is still black for a reason outside the
shaders lowered so far: the game's video path runs on the tensor cores.

- `runtime/ov_movie22.c` decodes the cinematics with dav1d (linked statically from MSYS2, BSD-2, row in
  `THIRD_PARTY_NOTICES.md`): the title is "video/av01" 1920x1080 at 60 fps, a 64 s stream with a key frame every
  321 samples (`nn::movie::Buffer` flag 4 marks those, not the end of the stream, which is an empty flagged buffer).
  Frames come back to the game as NV12 in the buffer it registered (it reads planes 0 and 1 only), with the
  sample's time stamp. Audio is still dropped. The decode runs on the game's decode thread; the game then runs
  at about 14 fps there, to be profiled (dav1d on 4 threads, the NV12 interleave, or the game waiting on frames).
- The lowering grew: SUST (bindless store, the handle loaded from the constant word), LEA and IMAD.X carries
  through comparisons and predicate adds (the translator ignores Maxwell LEA's carry predicate and X), the
  uniform register budget counts registers read before any definition, uniform pairs are allocated adjacently
  in both the pool and the liveness mode, the swap register for uniform predicates comes from the liveness
  allocator when the pool is full, and only the predicate fields an op has are rewritten. Corpus: 13,514 of 14,363
  lower (94.1%), all translate.
- What blocks the picture: every shader of the video path (19 compute programs, dumped by the host to
  `shader-cache/<key>.fail.{ctl,code}` for `tools/sm86_dis.py`) uses the tensor-core instructions: HMMA (0x03c,
  4,478 instructions), LDSM (0x03b, 1,670) and MOVM (0x03a, 24). They are the game's neural upscaler (the SDK
  list names "SMAA_For_NDSR"; the swapchain is 2560x1440 while the movie is 1080p), and they are not in the RomFS
  archives the census reads (nor in the ExeFS modules; the raw `romfs.bin` has no match either, so they sit in a
  compressed file the census does not visit, or the game builds them). NVN2 device property `0x67`, kept by the
  game as a capability flag and required by its video surface setup, is most likely "supports tensor cores".
  Next: lower HMMA.16816 / 1688 f16 (LDSM's matrix loads into per-lane fragments, the warp-cooperative product
  through SHFL and FFMA, MOVM's transpose) in front of the translator, or find the non-tensor path if the game has
  one (with `0x67` = 0 the player aborted in its surface setup; the shader's decision may lie elsewhere).
- Reaching the game proper: `OOT_INPUT_SCRIPT` (lines "<ticks> A 10") passes the title; the file select and the
  opening are cinematics too, so they need the same path.
- The intermittent death right after the first frame (one run in three, exit 40 or 5): the crash handler's dump
  (`main.c`) caught two. One executed from the thread's own stack while logging a guest `Break` from
  `sdk+0x1a5560` (the SDK abort), the log line itself garbled; the other executed from the heap region of the
  `Cpu` structures with `rt_svc+0x59` (after `svc_body`: the host-depth bookkeeping, `kern_preempt_pending` /
  `kern_host_left`) as the return address and lifted `main+0x3779bc0` below it. Both point at the asynchronous
  preemption (`kernel.c`, the interrupt trampoline that pushes the interrupted thread's registers on its stack).
  Test: `OOT_ASYNC_PREEMPT=0` over several runs; then read the trampoline against a thread suspended inside host
  code.

## Status (2026-11-06, later): the title frame decodes, the upscaler does not

The movie frame reaches the GPU and converts correctly: the first compute pass of the game's video path writes a
1920x1080 RGBA8 image of the title cinematic (Link on Epona by the river; dump `build/logs/dump-93`, pass 0000,
147,050 distinct colours). What follows it, the neural upscaler's twelve kernels and the final 2560x1440 pass, still
produces a near-constant image and then zeros, so the screen stays black. Found and done on the way:

- The tensor-core instructions are emulated in the lowering (`runtime/sm86_lower.c`, "tensor-core instructions"):
  LDSM (1, 2 or 4 matrices, plain or transposed) through SHFL.IDX of the row addresses and LDS; HMMA.16816.F16.F16
  through the PTX fragment layouts (lane = 4g + t), SHFL.IDX and HFMA2 (f16 accumulation, the tensor core accumulates
  wider); MOVM through two SHFL and a PRMT. Also added: SUST (bindless store, the handle a dword index in the
  constant word as TEX's), HFMA2 (all forms, .RELU through HSET2.GT.BF and HMUL2), F2FP (two F2F.F16.F32 and a
  PRMT), HMNMX2 (HSET2, HADD2, HFMA2), BAR.SYNC. All 19 kernels lower and translate; the corpus stays at 13,514
  lowered, 0 translation failures.
- NVN2's constant bank 0 for compute is not Maxwell's driver bank. `nvnCommandBufferPushComputeConstants(cmd, 0,
  offset, bytes, data)` (an NVN2 entry point the host only stubbed) pushes the kernel's parameter block, and the bank
  reads as: 0x10 reserved bytes (the dispatch size is put there), the pushed constants, then the bound resources in
  slot order, storage buffers {u64 address, u32 size, u32 0} for control[0xb2] slots, texture handles (u64) for
  control[0xb3] slots, image handles (u64) for control[0xb6] slots (the counts are bytes of the control section;
  control[0xb8..0xba] holds the bank's size). Measured on the kernels: the final pass pushes 0x54 bytes and reads its
  storage slot 2 at 0x90, textures 0 and 3 at 0xa0 and 0xb8, image 1 at 0xc8; the NV12 pass pushes 0x40 bytes and
  reads textures 1 and 2 at 0x58 and 0x60, image 0 at 0x68; the kernels without a push read storage slots 0 and 1 at
  0x10 and 0x20. The host records the push as OP_PUSH_CC, builds the bank at each dispatch (`vk_exec_draw.c`
  nvn2_c0_fill_cs), consumes the push with the dispatch, and resolves the shader's bank-0 words back to the bound
  slots for its descriptors (nvn2_c0_slot, cbuf_handle, ssbo_info). The graphics stages still get Maxwell's driver
  bank; the game's vertex shaders read 64-bit addresses at c0+0x60, which is the same scheme and will need the push
  (nvnCommandBufferPushGraphicsConstants is still a stub) when a draw depends on it.
- Diagnostics added: `OOT_BIND_LOG=1` logs the compute stage's binds, pushes, dispatches and inline uniform updates
  from frame 290 on; `OOT_PROG_DUMP=1` hex-dumps the memory around compute programs' control sections at
  SetShaders; the frame dump prints each stage's bound texture, image and uniform slots per pass and resolves texture
  declarations as the replay does (`vk_exec_res_handle`), so storage images written by dispatches get their PNGs.
- The death after the first frame, one run in three, is the SDK's own abort in
  `nn::os::detail::MultiWaitImpl::InternalWaitAnyImpl` (sdk+0x167be0): after a holder's virtual IsSignaled (vtable
  +0x18) returns true, it aborts when its count of found holders reaches the caller's capacity (the compare before
  the jump to sdk+0x167ec4, which calls `nn::diag::detail::AbortImpl()` through the GOT). The host then dies with
  the abort's return address as the instruction pointer (`executing 0x7005367ec8`, the LR of that call, rcx holding
  the Cpu), before `AbortImpl(const char *, ...)` is reached (an observer on it, `ov_oot.c` obs_abort_impl, never
  fires). Two bugs: more holders report signalled than the game expects (an nn::os event or timer the host leaves
  signalled at start-up, to find), and the abort path crashing instead of reporting. `OOT_ASYNC_PREEMPT=0` does not
  prevent it.

Next (as of that section): find where the upscaler's values go wrong. Resolved in the section below.

## Status (2026-11-06, night): the cinematic plays

The title cinematic renders: the AV1 frame decoded by dav1d, upscaled to 2560x1440 by the game's neural network
through the emulated tensor-core instructions, composited with its subtitles (`build/logs/run-184.png`, 27,548
distinct colours). At 21 fps while the cinematic plays, 60 fps before it. Six distinct defects stood between the
black picture and this, found one after the other with the tools listed at the end:

- The translator's dead-node removal (`Optimizer.RemoveNode`) followed into any producer whose use count reached
  zero, even one still live through another destination. SHFL defines a value and a validity predicate; dropping the
  dead predicate copy stripped the use lists of the shuffle's lane index, and the next pass deleted the index
  computation: every emulated LDSM and HMMA shuffled from an undefined lane. Patch 0013 follows only into producers
  that are dead through every destination.
- PRMT, the byte permute, was an unimplemented logging stub in Ryujinx in every form (R, I, C, Rc). Half the lowered
  programs use it (packing halves), and the emulation builds every half pair with it. Patch 0014 implements it; a
  constant selector (the usual case) is resolved at translation time, the half-word packs 0x5410 and 0x7632 to two
  instructions.
- With those two fixed the kernels' SPIR-V grew to 16-22 MB (over a million instructions; the NVIDIA driver took 20
  to 30 s to build each pipeline): every emulated shuffle was a helper-function call with four pointer arguments.
  Patch 0015 emits SHFL.IDX with the plain mask (clamp 31, no segment) on a 32-lane subgroup as the subgroup shuffle
  itself, the lane index masked to the subgroup. The pipelines still take 0.5 to 30 s to build the first time (the
  driver's disk cache keeps them); the emulation's instruction count is the next performance item.
- The uniform-predicate swap (`sm86_lower.c` lower_one): when a program uses all seven predicates, a uniform
  predicate is swapped into the predicate of the same number around each instruction naming it, and the game's
  predicate restored after. The first neural-network layer ends its tile loop on `PLOP3 P0 = UP0`: the restore put
  the old P0 back over the result, the loop ran until its counters wrapped (2.2 s per dispatch, measured with the
  kernel probe) and the GPU hit Windows' 2 s timeout, reported as a device loss with the instruction pointers of
  every killed warp. The swap now leaves the instruction's own predicate outputs alone.
- The upscaler's final pass stores its 2560x1440 picture into the game's sRGB output texture, which Vulkan cannot
  make a storage image; the host bound a dummy. `vk_texture.c` now creates such images with EXTENDED_USAGE and the
  STORAGE usage, and `vk_exec_draw.c` get_view stores through a view of the UNORM sibling (`vk_unorm_of_srgb`).
- The lowering put the scheduling words of graphics programs at absolute multiples of 32 bytes from the block
  start, but Ryujinx counts them from the end of the 0x50-byte header: every third instruction of every vertex and
  fragment shader sat where the decoder skips a scheduling word, and the scheduling word was decoded as an
  instruction. The compositing quad's texture fetch was one of them. `addr_of` now counts from the header's end.
  (The pipeline check `module_binding_missing` in `vk_pipeline.c` also had to learn that a declared but unreferenced
  binding is not a layout error: the translator declares a constant buffer whose binding it reserved even when the
  load was removed.)

Also on the way: the game's movie worker feeds samples whenever it has them without consulting the input event;
decoding each as it came ran the whole stream through dav1d in seconds, the ready ring dropped pictures and the game
waited forever for the frame of a sample it had sent. `ov_movie22.c` now queues the samples and decodes them as the
ring has room. The SHFL emitted by the emulation sends its validity predicate to PT (it clobbered P0). The device
fault handler keeps the vendor's crash blob (`fault-vendor.bin` next to the log), compute pipeline builds over 100
ms are logged with their SPIR-V size.

Tools that paid off, all kept: `oot.exe --probe-tensor` (a hand-assembled SM86 kernel: LDSM x4, LDSM x2 transposed,
HMMA.16816.F16 against a CPU reference, through the lowering, the translator and a real dispatch);
`oot.exe --probe-cs <dir>` (one of the game's compute kernels dispatched in isolation with stand-in resources and
timing: ctl, code, bindings.txt; the kernel's weights from an `OOT_DUMP_BUF` snapshot); `OOT_DUMP=dispatch` (the
frame dump armed on the first compute dispatch); `OOT_SKIP_DISPATCH_FROM=<n>` (bisecting a device loss down to a
dispatch); `OOT_CS_DUMMY_TEX=1`; `OOT_MQ_LOG=1` (nn::os LightMessageQueue and EventType traffic with threads and
call sites, which found the movie worker's stall); `OOT_SM86_STUB=hmma|ldsm|all` (the emulation replaced by zeros,
for timing); `OOT_THREAD_DUMP=s<seconds>` with host backtraces (the render thread inside `nvgpucomp64.dll`).

Open: the frame rate (21 fps in the cinematic: the emulated kernels, and the decode on the game's thread); the
start-up crash, now about one run in two (the SDK abort in MultiWait, see the previous section); the 656 skinning
vertex shaders the lowering refuses; the mesh stages. The game has not yet been taken past the cinematic.

## Status (2026-11-06, late night): the title screen as the player sees it

Watched with sound for the first time (the user), three things looked wrong. All three are settled:

- **The game froze and its music stopped for a while.** It was out of focus: the host reports the window's focus to
  the game as the console reports the HOME menu, and the game pauses itself. By design. `OOT_ASSUME_FOCUS=1` pins the
  focus for watched test runs. The title movie has no sound track: no run has ever created a movie AudioDecoder.
- **The cinematic ran at a third of its speed.** The game takes one movie picture per game frame and does not look at
  the time stamps, so at 21 fps the 60 fps movie played at a third of its speed (a pacing line every 300 pictures:
  5 s of video in 15 s). `ov_movie22.c` now hands pictures back on a playback clock and skips the ones already
  late. dav1d runs on a host thread per decoder, which signals the output event; the events are interprocess
  system events for that. The frame rate stays at about 22 fps. Decoding was not what limited it.
- **The logo was pixelated.** Its ASTC blocks were decoded right (ARM's astcenc agrees texel for texel) but put in the
  wrong places. Inside a GOB the Switch 2 puts the 16-byte column at 64 bytes and the second row bit at 32, the
  reverse of the Tegra X1. This was found by matching each block's edges against every other block in the stored
  order, and holds for the ASTC logo (16-byte blocks) and the BC4 masks (8-byte blocks). `tex_deswizzle.c` applies it
  to elements of 8 and 16 bytes. Smaller elements keep the X1 order: no game data has settled them yet.
  `OOT_GOB_SWITCH2_MIN=1` (or 2, 4) tries the Switch 2 order for them, and `OOT_TEX_RAW=<w>x<h>` writes a texture's
  raw blocks for such checks. Frame dumps now also write the alpha of 8-bit RGBA images (`..._a.png`).

Getting to the title menu needs an A press (`OOT_INPUT_SCRIPT` with `600 A 10`).

## Status (2026-11-06, end of the night): the title runs at 60 fps on the tensor cores

The cinematic was GPU-bound: 48 ms of GPU time per frame, all of it the upscaler's kernels, each emulated HMMA about
116 Maxwell instructions (32 shuffles, 32 half FMAs that the translator expands further). `OOT_CS_TIME=1` showed the
time spread over seven kernels. HMMA now runs on the host's tensor cores through VK_KHR_cooperative_matrix:

- `--probe-coopmat` found f16 16x8x16 subgroup matrices on the RTX 4070 and measured their per-lane element order:
  HMMA's register fragments, except that A's four registers go in as 0, 2, 1, 3. No data moves between lanes.
- The lowering (`sm86_lower.c lower_hmma`) emits a marker for an unconditional HMMA of a compute program with
  whole-subgroup workgroups; translator patch 0016 decodes it into one IR operation with two results and emits
  `OpCooperativeMatrixMulAddKHR` with element inserts and extracts. `vk_caps.c tensor_cores` turns it on when the
  device has the shape, half floats and 32-lane compute subgroups (`OOT_TENSOR_CORES=0` keeps the emulation); the
  choice is mixed into the cache keys.
- The first full run drew garbage at 60 fps. Bisection (`tools/bisect_kernels.py`, after a first attempt whose
  allow-list never matched) found kernel 5d415995, and the isolated kernel on random data found the cause in the
  translator: its AST optimizer moved single-use memory loads to their use, past later stores, so the kernel read its
  weights after it had reused that shared memory. Patch 0017 keeps memory reads in place. The emulation hid the bug
  by luck of scheduling.
- Result: 4.3 ms of GPU time per frame, 60 fps (the game's cap), the movie on its clock with no skipped pictures,
  and screenshots scored as pictures (`build/logs/check-on.png` against the emulated `check-off.png`).

Open: spirv-val asks for the VulkanMemoryModel capability next to CooperativeMatrixKHR (the driver accepts the
modules; switching the translator's memory model touches every shader). The tools made tonight are in CLAUDE.md
("Running and checking") and the project skill `verify-shader-change`.

## Status (2026-11-06, first play): into a game, then black

The user played from the title into a new game with a Switch Pro Controller (`tools/run.py --seconds 3600`,
build/logs/run-243.log). The title, menu and opening cutscene run at a steady 60 fps. Two problems:

- **The opening's voice-over is silent.** The game is voiced: about 7,700 voice streams in nine languages, an English
  voice bank for 99 cutscenes including `Dm_Kokiri_0000`, the Deku Tree's opening. No audio service was refused
  and the renderer runs. Next: a run with `OOT_TRACE=fs` to see whether the game opens the voice bank and streams
  (if not, suspect the language the host reports, `ov_system.c ov_language`).
- **Gameplay is black.** When the opening hands over to the game, the screen goes black while the game keeps
  presenting frames at 60 fps (no crash, no stall). In the same moment the game starts calling
  `nvnCommandBufferDrawMeshTasks`, which is a stub that draws nothing, and only 51 distinct shaders were seen in the
  whole session. The world is most likely drawn with mesh shaders: the mesh stages (listed as open since the
  start of Phase 3) are now the blocker. Next: a frame dump of a gameplay frame (`tools/run.py --dump`) to confirm
  what is drawn and how much goes through mesh tasks. Reproduced unattended (run-244): A pressed every two seconds
  from the title and `OOT_MOVIE_SKIP=2` (`tools/run.py --skip-movies`) reach the mesh draws in about 25 s, and the
  screenshot is pure black.
- **Faint rectangle outlines in the cinematic** (the user, on a frame filled with blue sky): the upscaler's tiles
  show at their edges. Not yet measured whether the emulated HMMA shows them too (tensor cores round once, the
  emulation per step) or whether a tile border is handled wrongly.

## Status (2026-11-06, past the opening): the level renders, then the GPU times out

The black screen after the opening cutscene was not the mesh stages. The game marks a movie's end with an empty
buffer of flag 8 (flag 4 marks the first sample of a stream); `ov_movie22.c` took 4 for the end, so no movie ever
ended and the game waited for the end-of-stream frame forever (run-248 logged the flag-8 buffer). Past it, the game
aborted in `nn::prepo::AsyncContext::GetCompletionEvent` (a play report's context; `ov_system.c` now completes it at
once with a signalled kernel event attached through the SDK). With both, the Kokiri Forest level renders: about 80
passes, 744 draws and 126 dispatches a frame (run-257), for about ten frames, until a device loss with the GPU
timeout's pattern (one instruction pointer per killed warp: a shader that runs too long, as the upscaler's loop did).

Open, in order:
- (Done, the same night) The GPU timeout on the first level frames. `OOT_SKIP_CS=all` removed it (a compute kernel);
  `OOT_SKIP_CS=!<list>` (run only those) and leaving one out at a time found a pair, eb67c232 (one thread copying
  light data from its uniform block, sampling two textures) and c82d3cba (a volume kernel with counted loops), that
  hung only together. The frame dump armed on eb67c232 (`OOT_DUMP=cs:<id>`) showed its two textures unbound: the
  host placed NVN2's bank-0 resource tables after the pushed constants by the length the game pushed, but the shader's
  layout ends the tables at the u16 at control +0xba (eb67c232 ends at 0x40 with 1 storage and 2 texture slots and
  reads its storage slot at 0x20). With the base from +0xba every compute texture slot of the level frame resolves,
  the level runs (662 frames, A/B against the old rule: device lost after 14) and the Deku Tree's dialogue plays
  (subtitles over a still black scene). Also translator patch 0018: NMin/NMax/NClamp and a saturating, NaN-to-0 F2I,
  as Maxwell defines them (not the cause, but SPIR-V left them undefined).
- The scene behind the dialogue was black for two reasons (2026-11-06, dumps of frame 1500 with
  `OOT_DUMP_SKIP_RUNS=1`; the dump's pass cap is now 1200 and it notes draws and dispatches skipped for a missing
  shader, with the code hash `tools/kernel.py` prints):
  - The last steps of the frame, the 1080p-to-1440p kernel 0a9afab0 and the kernel after it 7034cdb8, did not
    lower; the frame showed only the subtitles drawn over an empty target. The lowering now has SULD (23 level
    kernels), REDUX (27), HSETP2, HMUL2, negated HMNMX2 operands and TLD4 with per-tap offsets and fp16 results.
    With them the level shows: the sky and the forest's shapes (`build/logs/level-up.png`), the geometry still
    mostly black.
  - The volumetric fog: kernel 1a62084a (8x4x4 groups, the 64x36x96 in-scattering volumes) writes only 0 and NaN,
    and the integration along depth and the fog's full-screen pass carry the NaN into every pixel of the scene.
    Its uniform blocks, textures and storage buffers are bound and plausible; the cause is open.
    `OOT_SKIP_CS=1a62084a` leaves the fog out for now.
  - The materials: most fragment shaders take their texture handles from memory (bindless: a handle loaded into a
    register, R2UR, then TEX through a uniform register), which Ryujinx 1.3.2 cannot resolve; they sample the
    placeholder. True bindless (a descriptor array over the texture pool, indexed in the shader) is the next big
    piece.
- 605 programs the lowering refuses in the level (`gfx_shader: translation failed`), so about 280 of 744 draws are
  skipped: unknown opcodes 261, `TEX unpacked operands` 128, scratch and predicate pressure 53, `RED address` 37,
  uniform datapath opcodes 35, and smaller groups (TLD, TLD4, HMNMX2, PLOP3, STG of wide zeros).
- The mesh stages: the level draws with `DrawMeshTasksIndirect` too (stubs).
- The movie start stutters for about 8 s (every other frame ~160 ms while the GPU needs 5-13 ms: CPU side), and the
  cutscene's audio stream runs 10-20 s off the picture (the user); the opening's voice lines are silent.
- (Done) `OOT_MOVIE_SKIP`: the cutscene follows the time stamps of the pictures it shows, not the movie's end alone
  nor its audio stream (the user: the cut to the game came before the lagging audio ended). The skip now hands back,
  once the game has sent its end buffer, one black picture stamped with the last sample's time, then the end; the
  quick route (`tools/run.py --replay build/logs/a-spam.txt --skip-movies`) reaches the level in about 75 s.
  The game has no button to skip the opening (the user).

## Status (2026-11-06, evening): the level's programs lower, the scene is still black

Worked offline: the cache keeps every refused program as `<key>.fail.ctl` / `.fail.code`, and lowering those (and the
stored inputs `in/*.in`, to catch regressions) with `build/tools/sm86_lower` takes seconds, against minutes per game
run. Of the 64 translation keys the level route refused (28 distinct programs), 62 lower now (commits 73ea4d7,
913bbfb): IMAD.WIDE and uniform-base loads with less scratch (the six vertex shaders near R255), PLOP3 and UPLOP3
over any PSETP-computable table (and a fix: the outer PSETP slot held the wrong input), RED and ATOMG on odd uniform
pairs, PLOP3 against swapped uniform predicates through registers, indexed IPA, FCHK (always the fast path), TLD with
texel offsets, LEA with a negated addend, stores of wide zeros, IABS with constant operands. The water renders now
(`build/logs/level-lower2.png`, run 359); the rest of the geometry is still flat black or white.

Left on the route: a fragment shader's TEX with bit 60 clear (NAK's `.scr`; its operand layout is not known here) and
an unknown opcode 0x19f in a compute kernel.

**Then the frame dump** (`--dump 900`, `build/logs/dump-361`): the level is a forward renderer. Pass 136 is a depth
prepass (217 draws, colour masked: `chmask 0`) that covers the whole view; pass 168 is the colour pass (112 draws,
depth EQUAL without a write) into the r11g11b10f HDR target, and its draws look right where they land (bark and
moss textured, `pass_0168_t0_..._tm.png`), but most of the scene, the ground among it, is missing there. Relaxing
EQUAL to LEQUAL changed nothing visible, so it is not depth invariance. The missing geometry is drawn with **mesh
shaders**: the run calls `nvnCommandBufferDrawMeshTasksIndirect` thousands of times (a stub; `nvn_procs.c` now logs a
stub's call count at powers of two) and the game's programs come in four stage sets by control byte +0x56 (now logged
once each by `nvn_api_obj.c prog_set_shaders`): vertex + fragment (0 1), compute (5), and **7 + fragment** and
**6 + 7 + fragment**: mesh (7) and task (6) programs. So the next piece is the mesh stages, not more of the lowering.
Neither the translator (Ryujinx 1.3.2 has no mesh or task stage) nor the replay supports them today; the two ways are
VK_EXT_mesh_shader (the 4070 and RX 9070 have it; a mesh execution model in the translator) or running task and mesh
programs as compute that write vertex and index buffers drawn with a pass-through vertex shader. `docs/mesh.md` is the
hand-off for it.

## Status (2026-11-08, late night): compute shared memory as the program declares it

Every compute program used to get 48 KB of shared memory (`answers_for` in `vk_exec_draw.c`), because the NVN2 control
section's field for it was not known. On AMD that LDS is reserved per workgroup whatever the kernel touches, so the
upscaler's one-wave kernels ran a couple of waves per WGP. The field is control `+0x64`:

- **Measured** over the census's 251 compute programs (`tools/shader_census.py`) and the 121 distinct ones play stored
  (`build/v131072*/shader-cache/in`, the upscaler's among them; they are not in the archives): `+0x64` is 0 in
  exactly the programs without LDS, STS or LDSM (221 of the census's, 99 of the stored ones) and non-zero in all the
  others (30 and 22), always above the highest immediate offset they use (`0x80..0x5460`). In the census it is a
  multiple of the workgroup size (96 x 84 = 8064, 512 x 36 = 18432, 128 x 12 = 1536), the shape of a
  `shared float data[N][local_size]`. The only stored program with shared memory instructions and 0 there is
  `--probe-tensor`'s hand-made kernel, whose control now says 768.
  By hand, 197ec50b (4608 declared): its LDS addresses are `((lane >> 2) * 40 + warp) * 2 + 0x900..0xb90`, at most
  3530 bytes; its LDSM rows `(lane & 15) * 0x90 + (lane & 16) + 0..0x60`, at most 2288 bytes. Task and mesh programs
  use the word for something else (non-zero without shared memory instructions); they keep their own layout.
- **Code**: `gfx_shader_cs_shared` reads it (at most 48 KB), `answers_for` answers with it, and the prepare path
  re-answers recorded compute pairs with it (records from before hold 48 KB). `Sm86Lowered.shared_insns` counts the
  LDS/STS/LDSM lowered: a program with some and 0 declared is translated with 48 KB and a log line (none in the
  game). `KEY_VERSION` 31.
- **On the RX 9070 XT** (`OOT_CS_INFO=1`): the upscaler's pipelines get 1.5-22 KB of LDS instead of 49152 bytes (the
  driver rounds up: 2592 to 3072, 21600 to 22016). The SPIR-V is the same size, but the driver's register choice
  changes with it: the 769,898-word kernel uses 152 VGPRs instead of 234.
- **GPU time per dispatch** (`OOT_CS_TIME=1`, title, `--alone`, on upstream main 17b8c4b: run-65 before,
  `build/v131072-up`, run-64 after; matched by code hash, since the input ids change):

  | Kernel | Groups | Shared | Before | After |
  |---|---|---|---|---|
  | 63f42c62 (x2 per frame) | 120 | 4320 | 2.35 ms | 1.13 ms |
  | c5379e8e | 5400 | 2592 | 2.62 ms | 0.67 ms |
  | d22b0e01 | 900 | 3360 | 1.74 ms | 0.22 ms |
  | 197ec50b | 80x180 | 4608 | 1.17 ms | 0.54 ms |
  | dfc7a190 (x2) | 720 | 2592 | 0.94 ms | 0.18 ms |
  | df00b2bd (x2) | 60 | 21600 | 0.70 ms | 0.73 ms |
  | 5aeb2e11 | 85 | 3072 | 0.39 ms | 0.20 ms |
  | 5d415995 | 8100 | 1280 | 0.23 ms | 0.04 ms |
  | df040d44 | 70 | 5120 | 0.21 ms | 0.12 ms |
  | baea7b2f | 97 | 3072 | 0.14 ms | 0.11 ms |

  Compute per frame 14.5 ms before, 6.0 ms after; the GPU time per frame (`OOT_TRACE=perf`) 18.3 ms before, 9.4 ms
  after, so the title goes from 30 fps (the pacer holds 30 while frames miss 60 Hz) to a steady 60. On the
  amd-coopmat base the night before (run-20, run-21, no pacer yet): 12.35 to 6.05 ms of compute, 15.95 to 9.6 ms of
  GPU, 52 to 60 fps. df00b2bd gains nothing: its 60 groups never filled the GPU, and the driver's register choice
  for it spills more (1120 bytes of scratch instead of 432).
- **Every compute program's input id changes** (the answers are part of it; the code hash does not). The ids the
  docs use as names, old to new: the upscaler's 63f42c62 ac616599, c5379e8e 789500e3, d22b0e01 12658545,
  dfc7a190 6f1847f8, 197ec50b e4cfcc02, df00b2bd 410807fe, 5aeb2e11 43c01576, 5d415995 c5b329fe, df040d44 fc1f969e,
  baea7b2f 0ea2765d, e1da4ca8 8bd75830; the level's ace1cec0 726b17cb, 3f15911a 7b872d67, 3ce2b98c a82c24ca,
  745220f3 3a1c39ef, 1a62084a 8fb5a18e, e6d5432f 145fb221, 406ecfce 866197aa. `SKIP_CS_DEFAULT` matches input ids,
  so its kernels ran again until it named the new ones (`vk_exec_draw.c skip_cs`): 3f15911a as 7b872d67 (and
  ace1cec0, skipped until #16, as 726b17cb); the log says "OOT_SKIP_CS: dispatches of ... skipped".
- **Checked** on upstream main (the `verify-shader-change` skill; `--alone` runs): `--probe-tensor`,
  `--probe-coopmat` and `--probe-vkmem` pass; the title at 30 s scores 0.46 both ways (`build/logs/lds2-title-lds.png`,
  `lds2-title-up.png`, side by side in `lds2-title-side.png`: the same scene, the faster build already showing the
  logo); no validation errors, no shader failures, no 48 KB fallback in any run. The level route (200 s, warm): at a
  fixed 30 fps (`OOT_PRESENT_INTERVAL=2`) a frame every 2 s from 120 s is a good picture 41 times of 41 both ways, the
  same scene drawn the same (`build/logs/lds2-level30-grid.png`, after over before), 11.6 ms of GPU a frame after and
  11.9 before. With the adaptive pacer both run the level at 46-49 fps and about half the frames are the black NaN
  frames of `docs/level.md` (19 of 41 before, 22 of 51 after): its rule (8 frames in a row over 18 ms) does not catch
  this GPU's 14-18 ms level frames, with or without this change. The amd-coopmat base gave the same pictures.

## Status (2026-11-08, night): HMMA on AMD's 16x16x16 matrices

On the RX 9070 XT (AMD's 26.8.1 driver) the title stayed black for minutes on a first run: the upscaler's HMMAs only
had NVIDIA's 16x8x16 shape to go to, so they were emulated into kernels of up to 3.0M SPIR-V words, and the largest
took 181.6 s to build (compute pipelines build synchronously; issue #9 has a 4.18M-word one that LLPC had not finished
after 14.5 minutes on an RX 9060 XT). They now run on the RDNA 4 matrix cores, and the title plays at about 51 fps:

- **What the driver offers** (`oot.exe --probe-coopmat`): only M16 N16 K16 at subgroup scope: f16 x f16 to f32 or
  f16, 8-bit integers to s32, and fp8/bf16 forms. No 16x8x16.
- **The element order**, measured by `runtime/shaders/coopmat_layout.comp` (each lane fills element e of each matrix
  with 16 lane + e and stores it; the probe prints the grids), 32-lane subgroups, 8 elements per lane: A's row
  l % 16 at k = 8 (l / 16) + e; B's column l % 16 at k = 8 (l / 16) + e; the accumulator's (f16 and f32) column
  l % 16 at rows 8 (l / 16) + e. RDNA 4's WMMA register layout. `vk_caps.c tensor_cores` measures it at every start
  (16 ms, only on devices without the 16x8x16 shape) and takes tensor mode 2 only when it matches
  (`coopmat_layout_16x16_ok`), so a device with another order stays emulated.
- **The mapping** (translator patch 0027, `GenerateMatrixMultiplyAdd16`; the lowering sets bit 48 of the MMA marker
  in mode 2 and the IR passes the lane id). The product is computed transposed, D^T = B'' x A^T, where B'' is B^T
  padded from 8 to 16 rows by repeating rows 0-7: then every lane holds a whole row of D (lanes l and l ^ 16 the
  same one). A^T, B'' and D each cross lanes in four subgroup shuffles, each round a permutation; the per-lane choice
  of what to send and where a received word goes is one rotation by the lane's offset (eight selects) instead of a
  select per round. The product is accumulated from zero in f32 and C added before the one rounding to f16.
  (HMMA's C and D pairs run along a row where AMD's accumulator pairs run down a column: C through the accumulator
  would have cost eight shuffles.) A first version computed D directly, with B's columns repeated; it needed 46-54
  selects per HMMA against 28-29 now, and its kernels built and ran a quarter slower. A CPU model of each version's
  shuffles and `--probe-tensor` (LDSM, HMMA into zero and in place, against the CPU) are exact.
- **Validity**: the shuffles read lanes by number, so mode 2 takes only compute programs whose X size is a multiple
  of 32 and creates their pipelines with REQUIRE_FULL_SUBGROUPS (`vk_pipeline.c compute_create`), which Vulkan asks
  of cooperative matrices in SPIR-V before 1.6; the start-up check also wants cooperativeMatrix, 16-bit storage, the
  Vulkan memory model (the measuring shader) and computeFullSubgroups. spirv-val's only complaint is the
  VulkanMemoryModel capability (open since 2026-11-06). The NVIDIA path is unchanged: mode 1 when the device has
  16x8x16, and its SPIR-V for all 11 kernels is byte for byte what the old lowering and translator made. (Mode 1's
  pipelines lack REQUIRE_FULL_SUBGROUPS too; left alone, untested without an NVIDIA GPU.)
- **Build times**, cold, in the game (`vk_pipeline: a compute pipeline of ... took`): the title's ten HMMA kernels
  take 35.8 s together (52 s with the first version), the largest 7.8 s each (884k and 1.31M words), and c5379e8e
  (190k words) 14.0 s. That one is the kernel's own: emulated it takes 16.6 s and spills more (`OOT_CS_INFO`, below).
  The driver's disk cache keeps them, so this is the first run only.
- **GPU time**: 12.3 ms of compute per frame (`OOT_CS_TIME=1`; 16.5 ms with the first version), 16.3 ms in all, so
  the title is GPU-bound at about 51 fps (19.6 ms per frame) where the RTX 4070 reaches the 60 fps cap at 4.3 ms. The
  top kernel, 63f42c62 (576 HMMAs in a loop), takes 2.0 ms twice a frame with 120 one-wave workgroups: it is the
  serial chain of each wave, about 80 instructions per HMMA (the RTX 4070 runs HMMA as itself). Every big upscaler
  kernel uses all 256 VGPRs and spills 0.1-1.4 KB to scratch.
- **Checked**: the title's screenshots score as pictures (noise 0.45-0.49, `build/logs/amd-tc-on.png`,
  `amd-tc-v2.png`); with four small HMMA kernels emulated (`OOT_TENSOR_ONLY` on the other eight) the picture at 30 s
  matches the all-tensor one at 41.9 dB PSNR, a mean difference of 0.69 of 255 (`build/logs/amd-mix-side.png`;
  frames of different moments score 18-24 dB). An all-emulated reference is not practical on this GPU.
- **Tools added**: `OOT_CS_INFO=1` (AMD, VK_AMD_shader_info) logs every compute pipeline's VGPRs, SGPRs, scratch and
  LDS, `=isa` also writes the disassembly to `build/logs/cs-isa-<words>.txt`; `OOT_CS_BUILD_ONLY=1` translates and
  builds a dispatch's pipeline and records nothing, so `--probe-cs <dir>` (bindings.txt `dispatch 1 1 1`) times a
  kernel's cold build without running it; `kernel.py --tensor-mode 2`, `run.py --exe`, `xlate_patch.py --build`.

Open, for AMD: the per-HMMA chain (done 2026-11-09, the next section); compute pipelines that build under `P.lock` on
the replay thread (issue #9). (The 48 KB of shared memory every compute program got is fixed: the status above.)

## Status (2026-11-09): AMD's HMMAs in pairs, operands kept in the AMD layout

On the RX 9070 XT, on top of the per-kernel shared memory of the status above, the title's upscaler now takes 1.8 ms
of GPU time a frame instead of 6.0 and the frame 5.2 ms instead of 9.4; without that change, on main, 4.2 ms instead
of 12.3 (52 fps GPU-bound, 30 under upstream's adaptive pacer), and the title holds 60 fps. The HMMA kernels build cold
in about 9 s instead of 36-52 s. Each comparison is one build, `OOT_TENSOR_NATIVE` switching the plan off. Kernels are
named by their input ids from before the shared-memory change, which changed every id (the status above lists both).

- **Where the time went.** 63f42c62's AMD code (`OOT_CS_INFO=isa`) was 46,000 lines: 576 WMMAs, 5,288 `ds_bpermute`,
  16,677 `v_cndmask` and 4,500 f16/f32 conversions, about 80 instructions of data movement per HMMA, on each wave's
  serial chain. The kernel's shape makes most of it redundant: groups of four HMMAs read the same A (an LDSM.x4 just
  before), each accumulates in place over a chain of 18 groups, and its 72 B fragments, loaded once by LDSM.x4.trans
  before the loop, stay in registers for all of it. Every HMMA converted A, B, C and D again, undoing the previous
  one's work.
- **Pairs** (`sm86_lower.c hmma_plan`, translator patch 0028 `GenerateMatrixMultiplyAddPair`). Two HMMAs of a block
  that read the same A, where the second reads nothing the first writes and nothing between them touches the
  second's operands, run as one 16x16x16 multiply, transposed as before: R = P x Q + S with P = [B1 | B2]^T, Q = A^T,
  S[n][m] = C_{n/8}[m][n%8], so R[n][m] = D_{n/8}[m][n%8], both results side by side and no padding. All four
  operands move the same way: word w of lane l is register l >> 3 of a list of four (A0-A3; B1 and B2's registers
  interleaved; C1's and C2's), read in lane 4 (l & 7) + w, four shuffles and two rotations each; D the inverse.
  The lowering emits two markers in a row (bit 49 on the first, which the translator holds back, bit 50 on the
  second); bits 28-32 carry the flags below.
- **Operands kept in the AMD layout.** A register set is a web of definitions and uses: a pair's D read back as the
  C of the next pair of its chain; an A that an LDSM.x4 loaded, which in the AMD layout is the 16 bytes at the lane's
  own row address (one LDS.128 instead of four shuffles and four loads); a B pair that an LDSM.x4.trans loaded, which
  is a lane's column gathered from eight row addresses (eight shuffles and eight 16-bit loads, what the plain trans
  load costs). A web stays in the AMD layout only when, along every path of the control flow, every instruction that
  reads its registers is a use expecting that layout and every definition reaching those uses produces it (a forward
  search from each definition, a backward one from each use, union-find over both); any other reader, a partial or
  predicated write or a use reachable from elsewhere keeps it in HMMA's layout, and its markers convert as before.
  `plan_view` gives HMMA, LDSM and PLOP3 their exact registers: `insn_regs` counts bits 64-71 as a register of every
  ALU form, which made every LDSM read R0 and every PLOP3 a B register, and would have blocked most webs. In liveness
  mode the second HMMA's result is kept live from the first on, so no scratch lands on it.
- **Coverage** (`SM86_PLAN=1` with the lowering tool; pairs, singles, then how many take A, B and C in the AMD
  layout and leave D in it): 63f42c62 288 pairs, A 272, B 280, C 272, D 272 (16 chains of 18 pairs: the first
  starts from zero, the last converts); df040d44 232 pairs, A 231, B 203, C and D 202; 5aeb2e11 335 pairs and 98
  single, A 341, B 288, C and D 239; df00b2bd 72 pairs, A 72, B 68, C and D 70; c5379e8e 50 pairs and 8 single, A 50,
  B 18, C and D 38; 197ec50b 22 and 2, A 11, B 10, C and D 5. 406ecfce and b62d9f75, which have no LDSM, pair and
  chain only.
- **Rounding.** `--probe-tensor` has a second kernel now: LDSM.x4 A, LDSM.x4.trans B, a pair into zero accumulators,
  a pair adding to them in place, a pair with B's halves the other way round, on integers (exact) and on values whose
  products need 13 fraction bits. With an f32 accumulator (C converted in, one rounding to f16 at the end, the
  default) all 512 halves are rounded once per HMMA, as with the old path; with an f16 accumulator (C inside, no
  conversions, `OOT_TENSOR_NATIVE=3h`) RDNA 4 matches rounding once on 335 of 512 and is never more than one ulp off
  (the emulation: 256, also within one ulp).
- **Measured** (title, `OOT_CS_TIME=1` and `--trace perf`, 60 s, the last 100 frames; one build):

  | `OOT_TENSOR_NATIVE` | the eleven kernels, ms a frame | 63f42c62, ms each (x2) | GPU, ms a frame | fps |
  |---|---|---|---|---|
  | 0: one HMMA per marker (the old mapping) | 12.3 | 1.99 | 15.9 | 52 |
  | 1: pairs, every operand converted | 8.84 | 1.51 | 12.4 | 60 |
  | 1h: pairs, f16 accumulator with C inside | 7.24 | 1.10 | 10.7 | 60 |
  | 2: pairs and accumulator chains | 5.39 | 0.83 | 8.85 | 60 |
  | 3: also A and B from LDSM (default) | 4.20 | 0.48 | 7.7 | 60 |
  | 3h: as 3, f16 accumulators | 3.39 | 0.20 | 6.96 | 60 |

  Levels 0 and 3 ran with `OOT_PRESENT_INTERVAL=0` (the game's 60 Hz interval, no adaptive 30 fps); the others never
  miss 60 Hz, so the pacer leaves them there. Paced, level 0 holds 30 fps and its kernels take 14.1 ms, as the GPU
  clocks down between frames; a build of 027dc78, before the pacer, measured 12.4 ms and 51 fps. Of the hand-off's
  candidates, pairs alone already reach the cap, an f16 accumulator with C inside saves 1.6 ms more, keeping the
  accumulators in the AMD layout instead 3.5 ms, and taking A and B from LDSM another 1.3 ms. 197ec50b (0.88 ms, 46
  HMMAs, 80x180 groups) and dfc7a190 (0.42 ms twice) are the largest now. The f32 accumulator costs 63f42c62 2.4
  times the f16 one: per chain of 36 pairs its code is 705 lines with 552 conversions, against 95 lines (2.6
  instructions a WMMA).
- **On the shared-memory change** (972cdb1 with this one, title, `--alone`, `OOT_PRESENT_INTERVAL=0`, 75 s, the
  last 100 frames; `build/v131072-combo`, which also has PR #19's pacer, off here): GPU time per dispatch, plan off and
  on, the new ids in brackets.

  | Kernel | Plan off | Plan on |
  |---|---|---|
  | 63f42c62 [ac616599] (x2 per frame) | 1.127 ms | 0.262 ms |
  | df00b2bd [410807fe] (x2) | 0.740 ms | 0.083 ms |
  | c5379e8e [789500e3] | 0.657 ms | 0.080 ms |
  | 197ec50b [e4cfcc02] | 0.540 ms | 0.443 ms |
  | d22b0e01 [12658545] | 0.220 ms | 0.118 ms |
  | 5aeb2e11 [43c01576] | 0.196 ms | 0.103 ms |
  | dfc7a190 [6f1847f8] (x2) | 0.183 ms | 0.095 ms |
  | df040d44 [fc1f969e] | 0.125 ms | 0.037 ms |
  | baea7b2f [0ea2765d] | 0.107 ms | 0.054 ms |
  | 5d415995 [c5b329fe] | 0.038 ms | 0.041 ms |

  The eleven kernels 5.99 ms a frame and 1.77 ms, the GPU 9.41 ms and 5.17 ms (`OOT_TRACE=perf`), 60 fps both. df00b2bd,
  which the shared-memory change left as it was (its driver register choice spilled more), drops ninefold. Cold
  builds in the same runs (`OOT_CS_INFO=1`, which gives the pipelines flags of their own): 9.0 s for the HMMA kernels
  with the plan, 52.1 s without (two of them built twice).
- **Size and build time.** SPIR-V words, old and plan: 63f42c62 884k to 310k, 5aeb2e11 1.31M to 704k, df040d44 770k to
  283k, 406ecfce 283k to 183k, df00b2bd 275k to 123k, c5379e8e 190k to 95k, d22b0e01 166k to 103k. Cold builds
  (`OOT_CS_BUILD_ONLY=1` with `--probe-cs`): 63f42c62 7.9 s to 2.3 s, 5aeb2e11 7.8 to 3.3, c5379e8e 14.0 to 0.38 (it
  no longer spills), df040d44 2.6 to 0.65, df00b2bd 1.8 to 0.94; the title's ten HMMA kernels 35.8 s to 8.7 s (7.3 s
  with f16 accumulators). 63f42c62's code is 11,300 lines (6,500 with f16): 288 WMMAs, 176 `ds_bpermute`, about 710
  `v_cndmask`; it still uses all 256 VGPRs and spills 608 bytes, all of it in the prologue that loads the weights.
- **Checked.** `--probe-tensor` (both kernels, levels 0-3 and 3h, and emulated), `--probe-coopmat`, `--probe-vkmem`
  pass. NVIDIA's mode 1 and mode 2 with the plan off lower and translate all 13 tensor kernels byte for byte as main
  does (the lowering tool and a translator of patches 0001-0027). The title's screenshots at 30 s score as pictures
  (noise 0.45-0.47: `build/logs/hm-plan-30.png`, `hm-plan16-30.png`, `hm-noplan-30.png`). Runs show the same picture
  only at the same frame rate and present: two default runs at 30 s match at 36.2 dB, their background identical to
  the pixel (the logo's fade differs). Of eight full-size presents around 30 s per run (`OOT_SHOTS=w29.4-30.0:0.016
  OOT_SHOTS_FULL=1`), the f16 accumulator's best match with the default is 46.5 dB, a mean difference of 0.38 of 255,
  at most 11. The plan off reaches other presents (51 fps unpaced), so both ran at a fixed 30 fps
  (`OOT_PRESENT_INTERVAL=2`, `build/logs/hm-series30-{3,0}`): their matching presents are 73.0 dB apart, a mean
  difference of 0.003 of 255 and at most 2, the picture of the old mapping.
  On the shared-memory change with PR #19's pacer (`build/v131072-combo`): the three probes pass; the title at 30 s
  scores 0.52 (`build/logs/combo-plan-30.png`, the grass loop); plan on and off at a fixed 30 fps match at 72.9 dB, at
  most 3 of 255 (`combo-series30-{3,0}`). The level route (`--replay build/logs/a-spam.txt --skip-movies`, 200 s,
  warm, a shot every 2 s from 120 s): with the pacer 30 fps and the scene drawn in 41 shots of 41
  (`combo-level-paced`, the same dark spot as `lds2-level30-*`); unpaced (`OOT_PRESENT_INTERVAL=0`) 50 fps, bound by
  the CPU, 11.6 ms of GPU a frame and about 2.6 ms of compute in 60 kernels, none of them HMMA kernels
  (`combo-level-unpaced`).
- **Not built: a re-layout through shared memory** (stores, a barrier, OpCooperativeMatrixLoad). Loading A and B in
  the AMD layout from the program's own LDSM tiles moves no data the program does not already load, needs no shared
  memory beyond what the kernel declares, and leaves no store-load round trip on the chain.
- `OOT_TENSOR_NATIVE` (diagnostic, mixed into the cache key): 0 off, 1 pairs, 2 and chains, 3 everything (default),
  an `h` for f16 accumulators. KEY_VERSION 31.

Open: the f16 accumulator saves 0.7 ms a frame (the kernels 18%) but rounds within an ulp of once rather than once;
native A loads come out as four `ds_load_b32` (the translator's LDS.128 is four word loads the driver does not merge);
63f42c62's prologue spills; 197ec50b, half of whose pairs still convert A and B, is the largest kernel now.

## Status (2026-11-08, evening): the flickering level and the occlusion counts

**Measuring flicker without a person.** `OOT_SHOTS=2390-2405:1 OOT_SHOTS_FULL=1 OOT_SHOTS_DIR=...` on the level
route (`--replay build/logs/a-spam.txt --skip-movies`, 75 s) saves 16 consecutive presents; the share of pixels that
change by more than 0.3 from one frame and come back the next (an A-B-A pattern) measures the flicker: 10-30% before
the fix, 0.2-0.6% after.

**Cause: conditional renders on occlusion counts** (`vk_exec_xfer.c`). OoT tests objects with samples-passed reports
(over 300 a frame, from `main+0x759ac` and `main+0x75ecc` in pairs 0x20 apart, and `main+0x65f44`) and draws them
under `SetRenderEnableConditional` (mode 1, the counter at addr against the word at addr+16). The replay evaluates the
condition on the CPU when it records the op: a report of the same submission not done yet counts as unknown and
renders. Those reports were kept in a ring of the last 256; the oldest fell out before their conditions were read, which
then read guest memory (an older count, mostly 0) and skipped the object, so whole objects (the Deku Tree's trunk,
roots, branches) vanished in some frames. The pre-merge build `aa53891` did the same. Now a table by address (65536
entries). Found with `OOT_COND_LOG=<frame>` (evaluations and reports); `OOT_COND_RENDER=1` (render every condition)
gives a stable but white picture, as the game also gates full-screen passes so.

**The frame rate it cost.** The level ran at about 57 fps because the bug culled objects that should have been drawn;
with every unknown count rendering, almost nothing is culled and the level runs at 35-38 fps, CPU-bound (`--trace perf`:
28 ms a frame, GPU 17 ms; not the pointer table: `OOT_GPU_POINTERS=0` gives 38.5). Using the previous frame's count of
the same address when this frame's is unknown brought 49 fps back but the flicker too: an object drawn in frame N
occludes its own proxy, its count is 0, it is culled in N+1, and so on (a two-frame oscillation). The console avoids
it by comparing in GPU order within the frame. Next: evaluate these conditions on the GPU
(`VK_EXT_conditional_rendering`, available on the test machine): copy each report's query result into a buffer slot
outside the render pass and wrap the conditional draws in begin/end; first establish what the word at addr+16 holds on
the console (here it reads 0, so with cumulative counts the condition would always pass, which cannot be the intent).

**Kernels.** `ace1cec0` now takes 0.025 ms (its loop bounds come from the uniform table, main's fcb2936) and its
black-and-white blocks are gone at frame 2400, but with nothing skipped (`OOT_SKIP_CS=none`) a later scene of the
`a-spam-long` route (frame ~4880) is black but for Navi, so `ace1cec0` and `3f15911a` stay skipped by default.

## Status (2026-11-07): the level's frame rate is one kernel

`OOT_CS_TIME=1` on the level route (run 398): one compute kernel, input id `ace1cec038d64d1f` (code hash
`80fb2f9325762992`, 8x8 threads over 64x64 groups: one thread per pixel of a 512x512 target), takes 1578 ms of GPU
time per dispatch, 99.9% of the frame's compute; everything else is below 0.4 ms. Skipping it
(`OOT_SKIP_CS=1a62084a,ace1cec0`, run 405) takes the GPU from about 450 ms to 10.5 ms a frame: 58 fps in the first
minute, the replay script reaches a cutscene (a Kokiri and Navi, textured and lit, `build/logs/level-noace.png`), the
background dark. Its first loop (0x900-0xcf0) runs over 0x30-byte records of the storage buffer at bank 0 + 0x60
(NVN2 table slot 0; in the dump `build/logs/dump-ace`, 0x18040 bytes: a 0x40-byte header and room for 2048
records), sampling a texture each, up to the count at the buffer's + 4 (`LD R3, [UR4 + 0x4]`); later loops are
bounded by word 0 of uniform blocks 4-7, zero in the dump. Two of its four textures resolve to the 4x4 default
texture (bank 0 + 0x80, + 0x88). Next: the count it reads (the readback `OOT_CS_READBACK=<frame>,all,dims=64x64x1`
did not catch its buffer, so it may not go through `ssbo_info`), and whether its record buffer and textures are the
right ones. Until then, `OOT_SKIP_CS=1a62084a,ace1cec0` gives a playable frame rate on the level for testing.

## Status (2026-11-06, the start-up crash): two host threads on one fiber stack

The death right after the first frame (about one run in two: an access violation executing a heap or guest address,
or in `kern_fiber_finalize`, often on two host threads at once) was not the SDK abort in MultiWait, nor the
asynchronous preemption (`OOT_ASYNC_PREEMPT=0`: 4 of 10 runs died, against 2 of 10 with it). Every dump had the same
guest stack: `nn::os` waits (`obs_ev_trywait`, `InternalWaitAnyImpl`) down to a `WaitSynchronization` (SVC 0x18)
whose return landed on garbage. `OOT_FIBER_LOG=1` (new: a line per InitializeFiber, FinalizeFiber, SwitchToFiber
and fiber return, with guest and host thread) showed the pattern before each death: SDK 22's `nn::async` coordinator
yields a fiber with `SwitchToFiber(nullptr)` on one worker, another worker picks it up, and the fiber returns there.
`fiber_main` then switched to the base fiber of the thread it had *started* on: clang took the address of the
thread-local `tcur` once at entry and kept it in a callee-saved register across `rt_call` (LLVM treats a
thread-local's address as fixed within a function). That base fiber was already running on its own thread, so two
host threads ran one stack. `kernel.c` now reads `tcur` after a switch only through the out-of-line `tcur_now()`
(in `fiber_main` and after `SwitchToFiber` in `fiber_switch_to`, which also wrote the context's host depth into the
thread it had left). Result: 20 runs of 12 s, none died (before: 8 of 12, 2 of 10, 4 of 10 in three series).

Also: the SDK's argument-less `nn::diag::detail::AbortImpl()` (the one `InternalWaitAnyImpl` calls) gets an observer
in `ov_oot.c` that, for the MultiWait abort, lists the holders by vtable; the observer on the four-argument overload
never fired because the SDK does not call that one there. Left as is: `hostfn_tracked` and `rt_original` keep the
thread they started on across a call that can move a fiber (`c->thread` before the call), so with asynchronous
preemption on the host depth of a moved fiber is counted on the wrong thread.

## Status (2026-11-06, the mesh stages measured): the level's scene is drawn by mesh tasks

With the start-up crash gone, the quick route (`--replay build/logs/a-spam.txt --skip-movies`, 110 s) reaches the
Deku Tree's dialogue over a black scene every time. Measured on the way:
- The NVN2 stubs now log their call count at 10, 100, 1,000... (`nvn_procs.c` NVN_STUB). In about 40 s of the level
  the game made 10,000 to 99,999 `nvnCommandBufferDrawMeshTasksIndirect` and 1,000 to 9,999
  `nvnCommandBufferDrawMeshTasks` calls (run-85), so at least about ten mesh draws a frame, against about 100
  ordinary draws.
- The frame dump of frame 2000 (dump-84) has no scene pass at all: 59 dispatches and 86 render passes, the largest
  960x540 (10 passes, 36 draws), then the 2560x1440 composition with the subtitles. The mesh draws, being stubs, do
  not reach the dump; everything the scene is made of goes through them.
- 110 distinct programs fail to lower in the level (51,824 log lines, the same programs retried): unknown opcodes
  57, scratch pressure 17, `RED address` 10, uniform datapath opcodes 8, `PLOP3` 6, and smaller groups.
- The corpus holds 12 mesh-pipeline programs, 9 mesh and 3 task, all from the agl package
  (`ApplicationPackage.Nin_Ounce_NVN2.release.sarc.zs/AglShader.sharcb`), 16 to 616 instructions: a few generic
  mesh programs feed the materials' thousands of pixel shaders. None lowers: the stages write their outputs (and the
  task stage its payload) with `ISBEWR` and read their inputs with `ISBERD`, whose immediates are undocumented. The
  smallest mesh program, 28b308906146a98b (112 instructions, workgroup 32), reads `ISBERD [0x0]`, writes
  `ISBEWR 0x3ff` and `0x30a` (likely the vertex and primitive counts), then per vertex `0x4`/`0x6`, `0x8012`/`0x8018`,
  `0x100ff`, `0x18014`, `0x20000`/`0x20008`, `0x28001`/`0x28009` (an attribute word address with what may be a
  component in bits 15-17) and per primitive `0x40d`, `0x505`, `0x607`, `0x707`, `0x805`, `0x90f`, `0x8002`, `0x3`,
  `0x1000a`, `0x1800b`. The task program 9d61d62aa9337355 (16 instructions) writes `0x1001` and `0x400`.

What the mesh stages need, in order: decode the `ISBEWR`/`ISBERD` address encoding from these programs and what the
game draws with them; lower them (`sm86_lower.c`) to an output form the translator can take; a translator patch for
SPIR-V's `MeshEXT`/`TaskEXT` execution models (per-vertex and per-primitive output arrays, `SetMeshOutputsEXT`, the
task payload, `EmitMeshTasksEXT`), which Ryujinx 1.3.2 lacks; and on the host `VK_EXT_mesh_shader` pipelines with
task and mesh stages and `vkCmdDrawMeshTasks*EXT` for the eight NVN2 entry points, still stubs. The first step is the
uncertain one.

Also fixed: `tools/shader_census.py --lower` lowered nothing when the repository's path held a space (the helper's
batch request is split at spaces); it now passes paths relative to the output folder.

## Where the port stands

`oot.exe --seconds 15` runs the whole start-up of OoT 1.1.1 without a crash: 82 threads, 10,988 IPC messages, 1,485
file opens, saves mounted, NPLN offline, the audio renderer at 48 kHz, 3,661 shader programs set up through NVN2's
program builder, three frames presented at 2560x1440 (`build/logs/run-45.log`). Then it stalls in the third frame
(51.7 s) because no shader translates:

```
[host] gfx_shader: translation failed (-): bad control section
[host] vk: a stage 0 shader of program 4136 failed to translate: draws with it are skipped
```

Draws with failed programs are skipped, the game waits on GPU work that never completes, nothing is drawn. Everything
below the graphics API is believed to work; every remaining known problem is in this one area or is minor (see "After
the shaders").

## Why every shader fails

The host's shader path (`runtime/gfx_shader.c`, `runtime/vk_exec_draw.c`, `tools/xlate/`) is TotK's, for Switch 1:

- `gfx_shader.c` `req_init()` reads the c1 offset at `control+0x700` and the code length at `control+0x704`, and
  `CONTROL_SIZE` is `0x880`: the cache key, the `.in` files, `build_code_hash()` and the async job copies all take
  0x880 bytes from the control pointer. `vk_dump.c:585` reads `+0x700` too.
- The NVN2 control section is 224, 248 or 280 bytes (`0xe0`, `0xf8`, `0x118`). Reading `+0x700` reads whatever lies
  after it, so the length check fails and every program is rejected before translation.
- The code is Ampere SM86 (the Switch 2's T239 GPU): 128-bit instructions, opcode in the low 12 bits of word 0, no
  Maxwell scheduling words. The vendored translator (`tools/vendor/ryujinx`, Ryujinx 1.3.2, MIT) decodes Maxwell and
  Pascal 64-bit forms only (`src/Ryujinx.Graphics.Shader/Decoders/InstDecoders.cs`).
- The translator itself is not built here yet: the log says `translator identity 0000000000000000 (none: no
  translator)`. `.NET 10.0.403` is installed; `python tools/build_xlate.py` (Windows venv) builds
  `build/xlate/oot_xlate.exe`. MP4's `mp4_xlate.exe` next door is the same source and would do for a first smoke test.

So there are two separate jobs: the control-section layout (small, data-driven) and an SM86 front end (large).

## What is measured about the NVN2 control section

From the decompressed `Shader/Model.Product.110.product.Nin_Ounce_NVN2.bfsha` (320 MB; zstd with the `zs.zsdic`
dictionary from `Pack/ZsDic.pack.zs`): 8,517 blocks found by the magic `0x98761234`, 20,000 of them surveyed by
per-dword variance. The first 0xe0 bytes are the fixed part; the block's own size sits at `+0x30` and is repeated at
`+0x98` and `+0xd4`; the tail after `+0xe0` is 0, 0x18 or 0x38 bytes of extra records.

| Offset | Seen | Reading |
|---|---|---|
| `+0x00` | `0x98761234` | magic, as on Switch 1 |
| `+0x04`, `+0x08`, `+0x0c` | `0xc9`, `0x14`, `0xe0170` in every block | constants (version words) |
| `+0x18`, `+0x1c` | `0x220014`, `0x288040` in every block | constants (SDK 22.x / NVN2 version?) |
| `+0x20..+0x27` | unique per block | a 64-bit hash |
| `+0x28..+0x2f` | 7,832 distinct over 8,517 | a second 64-bit hash, shared by a few blocks |
| `+0x30` | `0xf8` (5,900), `0xe0` (2,177), `0x118` (315) | **control section size**; repeated at `+0x98`, `+0xd4` |
| `+0x38` | 402 values, `0x6e80..0x7000` typical | code size candidate (multiple of 0x80) |
| `+0x3c` | `0x14` (5,900), `0` (1,906), `0x20` (394) | tracks `+0x30`; `+0xdc` has the same classes |
| `+0x40`, `+0x44` | rise with `+0x38` (`0x9e80, 0x9f00, 0xa000`) | sizes with headers, or offsets of c1 and end |
| `+0x4c` | 150 values, `0x5a..0x60` common | register count or similar small per-shader number |
| `+0x50..+0x60` | small flags, `0x10000` common | stage/feature flags |
| `+0x64` | compute: 0 or `0x80..0x5460` | **compute: the shared memory the program declares**, in bytes (2026-11-08: 0 in exactly the programs without LDS/STS/LDSM, else above every immediate offset they use; in the census a multiple of the workgroup size) |
| `+0x64..+0x70` | `0x3f800000` in a few blocks | floats (1.0f) |
| `+0xa4` | `0x1000001` or `0x1000101` | flags |
| `+0xbc`, `+0xc4`, `+0xc8`, `+0xd8` | `0xe0` always | size of the fixed part |

The survey script is a few lines: find each magic, measure to the next, tabulate each dword. Not yet known and needed
first: which fields are the **code length**, the **c1 offset** (the constant bank the translator needs for `BRX`
jump tables) and the **stage**, and whether an early-fragment-tests flag exists (Switch 1: `control[0x718] & 1`).
Two ways to pin them, both cheap:

1. The archive's own tables. BNSH's shader code section records per program the code and control offsets and sizes
   (TotK's `refs/totk-pc-recomp/tools/shader_census.py`, from MacNativeGames' `prepare_shader_seed.py`, MIT, parses
   Switch 1 BNSH; the container is unchanged here). Match those sizes against `+0x38`/`+0x40`/`+0x44`.
2. The game at run time. `prog_set_shaders()` (`nvn_api_obj.c`) logs `data0`; add a once-only dump of the 0x120 bytes
   behind `control` and the size of the pool mapping behind `data`, and compare with the bytes that follow the code
   (the program's own end is where the code size says).

Note the game hands over the Switch 1 `NVNshaderData` form, `{data, control}` of 16 bytes
(`nvnProgramBuilderSetShaders`, built on the stack at `main+0x14e2468`), so the host side needs no ABI change.

## What is known about the code

- Every stage ends in a 128-bit `EXIT`, opcode `0x94d`; none has Maxwell's. Instruction words are 16 bytes, the opcode
  is `word0 & 0xfff` (Lichii's `instruction.h` uses exactly this `Bits(0, 12)`).
- Phase 0 measured a 0x60-byte program header before the instructions, against Maxwell's 0x50-byte SPH. Lichii's
  environment reads `sizeof(sph)` (0x50) from the program base for Switch 2 titles too, so settle the header size on
  the corpus before anything else: the first instruction is where a plausible opcode stream begins.
- The translator job format (`tools/xlate/Program.cs`) is "graphics: 0x50-byte SPH + instructions, compute:
  instructions"; `gfx_shader.c` skips `CODE_SKIP 0x30` of NVN code-block header before that for graphics stages.
  Both constants must be checked against the NVN2 layout.

## Approach: lower SM86 to Maxwell in front of the existing translator

Recommended. Lichii (`src/shader_recompiler/frontend/ampere/lower_to_maxwell.h`, GPL-3.0, read for behaviour only) runs
four Switch 2 titles this way: it does not reinterpret the bytes, it emits new Maxwell instructions for each SM86 one,
inserts the scheduling word every three instructions, maps the uniform registers (`UR0..`) onto high general registers
(R216 and up), keeps an address map so branch targets and `BRX` tables survive the re-layout, and hands the result to
the unchanged yuzu Maxwell front end. The alternative, a native SM86 decoder inside Ryujinx's translator, would mean
new decoder structs and emitter signatures for every instruction (Ryujinx's emitters are bound to the 64-bit field
layout), many times the work for the same output.

Ours would be a C module with no dependency on the translator, usable both in `runtime/gfx_shader.c` (lower, then
request the translation of the Maxwell code; cache keys stay over the original bytes) and in an offline tool over the
whole corpus. Sources of truth that may be copied, with a header and a `THIRD_PARTY_NOTICES.md` row:

- Mesa's NAK compiler, `src/nouveau/compiler/nak/sm70_encode.rs` (MIT): the Volta/Turing/Ampere encoding, field by
  field, and the reference Lichii itself names for its encoding knowledge.
- Ryujinx's `Decoders/InstDecoders.cs` and `InstTable.cs` (MIT, already vendored): the Maxwell target encoding.

From Lichii, only behaviour: which SM86 forms Nintendo's compiler emits for these engines, the pitfalls it records in
`NX2_NOTES.md` (row 54: widen until the whole dump lowers; row 93: save and restore P6 around uniform guards; row 121:
translation time), and its test cases under `src/tests/video_core/ampere_*.cpp` as a checklist of forms (uniform
datapath `R2UR`/`UP2UR`/`ULDC`, `LDSM`, `IPA` modes, formatted surfaces, `CALL.REL.NOINC`, 64-bit `UIADD3` carry,
`TMML`, attribute `0x5c`).

## Order of work

1. **Build the translator** (`python tools/build_xlate.py` from the Windows venv), so a lowered shader can be checked
   at once. Confirm with MP4's census shaders if needed.
2. **Corpus tool**: port `refs/totk-pc-recomp/tools/shader_census.py` to the NVN2 archives (`Model.bfsha`, the
   `Layout2Shader`, `ApplicationPackage` sarc, `.vsetb`, fonts; `tools/shader_inventory.py` is MP4's Retro-format
   tool and is replaced, not extended). Output under `build/v131072/shaders/`: control and code per program, plus an
   opcode histogram (`word0 & 0xfff`) over all code. The histogram is the work list for step 4.
3. **Control section** in `gfx_shader.c`: one `ctl_parse()` giving size, code length, c1 offset, stage and flags, used
   by `req_init`, `build_code_hash`, the `.in` writer/reader, the async job copies and `vk_dump.c`; `CONTROL_SIZE`
   becomes the block's own size. Bump `KEY_VERSION`/`IN_VERSION` (the formats change). The `--probe-gfx`-style census
   self-test in `vk_pipeline.c` (`build/v121/shaders`) and `gfx_shader_probe.c` need the new corpus path.
4. **SM86 lowering**, corpus-driven: implement by opcode frequency, with the offline tool reporting the share of
   programs that lower and translate; stop at every unknown form rather than guessing. Hard parts in order: the
   address map (sched words every three instructions change every branch target; `BRX` tables live in c1), the
   uniform datapath, predicates and the P6 guard, then texture and surface forms.
5. **Run the game** with `OOT_TRACE=gfx`. Expect the first pixels, then new failures: NVN2 procs still stubbed
   (`nvn_procs_stub.inc` logs the first three calls of each; 160 return 0, only three were reached so far), state
   objects whose NVN2 layout differs, and whatever frame 3 was waiting for.

## After the shaders

Known and parked, none blocking: the C++ unwinder reads past `subsdk0` into the gap before `sdk` (`QueryMemory`
should report it free); `--probe-ipc`'s sdk heap constructor address is MP4's (`ipc.c` `ctors_143`); the
`--probe-services` override list; installer tables (`installer/layout.py`, `tools/make_release.py`,
`tools/prepare_shaders.py`); `tests/diff/func.py` and `tests/jumptables_check.py` not run; MP4's stale `fnprof_hit`
stub and `"\mp4"` escape to report upstream.

## Working facts

- Build: `/mnt/c/dev/metroid-prime-4-recomp/.venv-win/Scripts/python.exe tools/build_game.py` (257 s), output
  `build/v131072/oot.exe`. Lift: `recomp/gen.py --exefs build/extracted/v131072/exefs --out generated/v131072
  --modules main,subsdk0,sdk`.
- Run, always with WSLENV or every `OOT_*` variable is silently dropped:
  `export WSLENV=OOT_TRACE:OOT_VOLUME:OOT_APP_POOL:OOT_CRASH_PEEK:OOT_BREAK_AT:OOT_BREAK_BRIEF:OOT_BREAK_WATCH;
  OOT_VOLUME=0 OOT_TRACE=gfx build/v131072/oot.exe --seconds 15 > build/logs/run-N.log 2>&1`.
- Debug: `OOT_BREAK_AT=main+0x...`, `OOT_BREAK_WATCH`, `OOT_CRASH_PEEK`, `build/logs/nvn-proc-got.json` (GOT slot to
  proc name), `OOT_TRACE=svc,host,ipc,fs,gfx`.
- Extraction: `python3.14 tools/extract_nup.py --verify` (needs `toolchain/zbic/nso_unzbic` from `tools/build_zbic.py`).
- Lichii is at `/mnt/c/dev/Lichii` (GPL-3.0): `NX2_NOTES.md`, `src/shader_recompiler/frontend/ampere/`,
  `src/video_core/shader_environment.cpp`. Behaviour only, never code.
- Nothing from the game is committed; everything extracted or decompressed stays under `build/` (the decompressed
  bfsha of this session lived in the scratchpad and is gone; regenerate it from the RomFS image).
