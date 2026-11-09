# Pointer access in shaders

Design note for the shader memory accesses the translator cannot place, written 2026-11-08 before any code. They
keep the level's culling chain from counting anything (`docs/mesh-tracker.md`, log of 2026-11-08), so the mesh
draws draw nothing. Update the Status section as it moves.

## The problem

SM86 code reads and writes memory through 64-bit global addresses (LDG, STG, ATOMG, RED). Ryujinx's translator has
no pointers: `GlobalToStorage` (`Ryujinx.Graphics.Shader/Translation/Optimizations/GlobalToStorage.cs`) traces each
address back to a constant-buffer entry that holds a storage buffer's base, and rewrites the access as a storage
buffer access. On Maxwell games the address nearly always comes from the driver's table in bank 0, so this works.
When it finds no entry, `TryGenerateMultiTargetStorageOp` builds a helper with no targets: loads return 0, stores and
atomics do nothing, and the only trace is the note "Failed to find storage buffer for global memory operation" in
the translation result (`<key>.res.json` in the shader cache). A helper with targets has the same tail: an address
outside every target is dropped too, with no note.

This game's kernels follow pointers they load from memory: the buffers of the culling chain hold GPU addresses
(pass 1's storage slots 2 and 3 start with 0x2a19e9000 and 0x29bb2d000). Measured on the level (dump 443, frame
4500):

- pass 0 (`ed1f533a`, 169x1x1), the head of the chain: 5 stores, 6 AtomicAdd and 3 AtomicMaxU32 dropped, 35 loads
  that read 0;
- every indirect kernel after it (passes 1-11): 3-19 loads that read 0;
- over the shader cache: 155 of 1340 compute programs have dropped accesses (24 drop a store or atomic), and so do 6
  of 11 task and 11 of 88 mesh programs (loads), and some vertex and fragment programs (loads). The totals may include
  results of older translators.

Pointers the CPU writes into a uniform block are already resolved on the host (`vk_exec_draw.c ubo_ptr`, the
terrain's pointer block). That cannot reach a pointer a kernel reads from a storage buffer, or one another kernel
wrote.

## What the host already has

- **GPU address = host address for imported pools.** `vk_memory.c import_pool` imports the guest pages at
  `(void *)import_lo` with VK_EXT_external_memory_host, aligned to `minImportedHostPointerAlignment` (4 KiB): a
  pool's NVN address is the address of its bytes in our process. The level frame has about 3,950 imported pools
  (4.6 GiB).
- **Physical and virtual pools.** A physical pool is one VMA buffer; a virtual pool maps 64 KiB pages to a physical
  pool and offset (`PoolRec.pt`). `vk_mem_resolve` turns any GPU address into a VkBuffer and offset.
- **VRAM mirrors** (`vk_memory.c`, "VRAM mirror of buffer-bound pools"): on a discrete GPU an imported pool the replay
  binds as a buffer gets a device-local copy. The replay syncs it from guest memory at its first use in a submission
  and writes back the ranges it knows the GPU wrote (`vk_mem_gpu_wrote`) at the end of the submission.
  `OOT_MEMORY=import` turns mirrors off.
- **Buffer device addresses**: `vk_device.c` reads `bufferDeviceAddress` and VMA is created with
  `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` when present; the pool buffers do not have
  `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` yet (`pool_usage`).
- **The translator**: `Spv.Generator` has the SPIR-V enums for physical pointers, but the generator sets
  `AddressingModel.Logical` and has no code for them (`SpirvGenerator.cs`). Our patches 0019-0021 already extend the
  SPIR-V side (mesh stages, SPIR-V 1.4).

## Design

Keep an access that `GlobalToStorage` cannot place as a real pointer access, through a table that turns the game's
GPU address into a Vulkan device address.

**The address table** (host, `vk_memory.c`). One storage buffer, device-local, two levels so it covers the 39-bit
GPU address space without a flat table: a top level of 512 entries (one per GiB) holding the offset of a second-level
block, or 0; a second-level block of 262,144 u64 entries (4 KiB pages, 2 MiB a block) holding the device address of
the page's first byte in the buffer the replay uses for it, or 0 when the page is not backed. 4 KiB because imported
pools are aligned to 4 KiB, not to the virtual pools' 64 KiB (a virtual page fills 16 entries). Built from the pool
index and rebuilt where `pool_gen` changes (pool created, finalized, virtual pages mapped), as a copy into the table
recorded at the start of the next submission. At the level's footprint (addresses at 0x25e000000-0x2a4000000 and
0x15xxxxxxxx) that is a few GiB of coverage, about 10 blocks, 20 MiB.

**The shader side** (translator patch). Where `GlobalToStorage` gives up (the no-target helper, and the tail of a
helper with targets), emit instead:

    page = table_top[addr >> 30]; entry = page ? table[page + ((addr >> 12) & 0x3ffff)] : 0
    if (entry == 0) { load: 0; store, atomic: nothing }     // as today, so an unmapped address cannot fault
    else            p = (PhysicalStorageBuffer uint *)(entry + (addr & 0xfff)); load / store / atomic through p

SPIR-V: `AddressingModel PhysicalStorageBuffer64`, capability `PhysicalStorageBufferAddresses`, `OpConvertUToPtr` to
a `PhysicalStorageBuffer` pointer, `Aligned 4` on each access; SPIR-V 1.5 has it in core (we emit 1.4 for mesh
programs, so 1.5 for programs that use this, or the `SPV_KHR_physical_storage_buffer` extension). The table is one
more storage buffer in the program's reflection (`res.json` `sbufs`, a reserved slot the host recognises, as the
support buffer is), bound by the replay like the others. 8- and 16-bit loads go through the aligned word and a
bitfield extract; 8- and 16-bit stores and atomics stay dropped in the first version, with their own note, until a
program is seen to need them.

**Mirrors and coherence**, the part that decides how far the first version goes. The table points each page at the
buffer the replay would bind for it: the mirror when the pool has one, else the imported buffer. Two gaps follow:

1. A mirror is synced from guest memory only at its first *bound* use in a submission, so a pool that a kernel only
   reaches through pointers may be stale in its mirror.
2. What a kernel writes through a pointer into a mirror is not written back (`vk_mem_gpu_wrote` never hears of it),
   so the CPU does not see it.

With `OOT_MEMORY=import` neither exists: every page is the guest's own memory, coherent with the CPU (and slow
over PCIe on a discrete GPU). The first version is built and checked in that mode. Mirrors come second, with GPU
feedback as the pool sets already do for textures (`docs/bindless.md`): every pointer access sets a bit per page in
a feedback buffer the submission copies out, and the host syncs (and writes back) the pools seen there from then on,
one submission late. Whether the CPU reads what the chain writes (statistics, readbacks) decides how much that lag
matters; to be measured with `OOT_MEMORY=verify`.

## Alternatives not taken

- **Teach GlobalToStorage to trace further.** The address is loaded from memory the kernel itself (or another
  kernel) wrote; there is nothing in the code to trace it to.
- **Resolve on the host, as `ubo_ptr` does.** Only works for pointers the CPU wrote into a block bound at the draw;
  these are in storage buffers and some are written by the GPU.
- **One Vulkan buffer over all guest memory**, so the pointer is an offset. Import sizes are limited, pools come and
  go, and mirrors make the backing per pool anyway; the table keeps the per-pool backing.
- **Binary search over a sorted pool list in the shader.** No table to maintain, but some 12 dependent loads per
  access in the chain's hot loops; the table needs one or two.

## Plan

| ID | Step | Done when |
|---|---|---|
| P1 | The address table in `vk_memory.c`: built from the pool index, updated with `pool_gen`, the pool buffers created with `SHADER_DEVICE_ADDRESS`; `OOT_GPU_POINTERS=0` leaves it out. A host check that walks it against `vk_mem_resolve`. | Every pool page in the level frame resolves through the table to the same buffer and offset as `vk_mem_resolve` (a self-check, logged once). |
| P2 | Translator patch: the fallback above for loads, stores and 32-bit atomics; the table binding in the reflection; `KEY_VERSION` bumped. | `tools/kernel.py xlate` of `ed1f533a` and `a3e2446d` passes `spirv-val`, and their notes no longer list dropped stores or atomics. |
| P3 | The replay binds the table for programs that declare it (`vk_exec_draw.c`, all stages). | `--probe-cs` of `ed1f533a` on captured inputs writes non-zero counts. |
| P4 | First light under `OOT_MEMORY=import`: the level route with `OOT_MESH_IND_LOG`. | The chain's indirect records are non-zero, the mesh draws execute, and the level shot changes; the verify-shader-change skill passes on the title. |
| P5 | Mirrors: per-page feedback from pointer accesses, sync and write-back of the pools it names. | The level renders the same with mirrors on as under `OOT_MEMORY=import`, and `OOT_MEMORY=verify` finds no mismatch. |
| P6 | Cost: GPU time of the chain with the table (`OOT_CS_TIME=1`), and the table's size and rebuild time. | Reported next to a passing screenshot. |

## Open questions

- Do the dropped accesses in vertex, fragment, task and mesh programs matter as much as the compute ones? P4 will
  show it for the level; the rest of the cache can be judged from the notes.
- Does the CPU read anything the chain writes through pointers (it decides P5's urgency)?
- Does the GPU need the 64-bit atomics some kernels may use (ATOMG.64)? None of the notes counted so far are 64-bit.

## Status

- 2026-11-08: written.
- 2026-11-08: P1 done (`vk_memory.c`, "pointer table"; `vk_mem_ptr_table` hands it to the replay). On the level
  route at frame 4000 (`OOT_GPU_POINTERS_CHECK=4000`): 3,950 pools, 1,337,961 pages (1,259,881 backed), every entry
  equal to `vk_mem_resolve`'s buffer and offset; 10 of the 16 default blocks in use. The pool buffers now carry
  `SHADER_DEVICE_ADDRESS` (imports allocate with `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`): 0 validation errors,
  frame rate unchanged. Entries name the imported buffer even when the pool has a VRAM mirror (P5). Next: P2.
- 2026-11-08: #12 (Z-TOON) merged (7d185f2) with a run-time route of its own, which goes first: translator patch
  0022 searches the stage's storage table in bank 0 for an address it cannot trace, 0023 skips a target that gets no
  binding, and the host binds what small bound storage buffers point at into free slots (`vk_exec_draw.c
  ptr_targets_add`, vs, fs and cs; `OOT_PTR_BIND=0` turns it off). The branch's own page table was left out for
  P1's; `vk_mem_guest_pages` gives P1's table to shaders through the support buffer at byte 1408, so a shader can
  read it without a storage binding (the P2 patch should use that rather than reserving a slot). `vk_mem_shutdown`
  frees the table. P2 is to be rebuilt on top of 0022/0023 (the edits made before #12 are not in the tree). Hookshot
  (`docs/level.md`) does both: a pointer-pool table at c0+0x880 for task, mesh and compute, and a page-table fallback.
- 2026-11-09: 0022's search made some fragment stages take minutes to build. A fragment shader of the Forest Temple
  reads a 1 KB record through one untraced pointer, at +16, +20, ... : 253 calls of the same 16-entry table search,
  which NVIDIA's driver inlined (4,000 guarded loads) and then optimised for 78-155 s per stage (RTX 5070, driver disk
  cache off, `vk_pipeline.c lib_slow_note` logs every stage library over 2 s; `OOT_SLOW_LIB_DUMP=1` writes its
  module to `slow-libs/` next to oot.exe). Every slow stage was one of these; the larger fragment stages without the
  search built in under 2 s. Translator patch 0029 emits the multi-target GlobalToStorage helpers (`...Multi_c...`)
  with `DontInline`: the slowest fragment library of the area now builds in 0.5 s, a cold load into the temple has no
  draw waiting for a pipeline (it had dozens, up to 107 s), and the next start's prewarm builds the area's 430
  pipelines in 0.4 s. Cost: GPU time per frame in the temple 11.6 ms against 10.9 (two runs each, `OOT_TRACE=perf`,
  noisy). Searching once per base pointer instead of once per access would win that back and make the shader smaller;
  not done. The pipeline workers went from at most 4 to a third of the logical processors (at most 12;
  `OOT_PIPE_WORKERS` up to 32), since a zone change brings dozens of new stage libraries at once.
