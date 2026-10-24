# Structural bottleneck, part 1: GPU-thread lock contention

Date: 2026-10-24. Branch `sol/spartan-profile-attribution`, runtime
`20261024-210208-e886769-dirty` plus the ps4_app extent-cache change. Opening combat
tutorial, bounded 220 s native runs at 1080p, `PS4_PAD_AUTOPRESS=4000`.

## What the CPU samples showed

A 5 s `sample` of gameplay before these changes (`cpu_1080.txt`, run
`run_20261024_174255_072672`) split the GPU-side threads as follows:

| Thread | Running | Waiting | Where it waited |
|---|---:|---:|---|
| GpuCommandProcessor | 52% | 48% | mutex inside `CopySparseSerial` (guest reads) |
| DrawPrep0–3 | ~40% each | ~60% each | the same mutex, from `ReadUdReg` / `GetParams` |
| DrawRec | 62% | 38% | the same mutex, `IsValidGpuMapping` |

Every guest-memory read the GPU threads made, including 4-byte user-data reads, took
the shim's API-table mutex. It also called `query_region`, which takes both VM locks,
and the per-thread extent cache held only one extent, so alternating between shader code,
descriptors and rings missed on almost every read. The second cost was
`GetParams<ShaderProgram>`: for every shader stage of every draw it allocated and zero-filled a
64 KB vector and copied 64 KB of guest memory into it to find the binary-info footer.

The earlier Spartan sample, in which the command thread spun in `WAIT_REG_MEM`, came from a
different phase. In gameplay, the new `regmem_wait_ns` counter shows 0.7 ms per frame of
label waits.

## Changes

| Change | File | Toggle |
|---|---|---|
| Whole-range lock-free read first; walk regions only for sparse ranges | `shim/gow3gpu.cpp` `CopySparseSerial` | none (fallback is the old walk) |
| API table read without the mutex (published once, never replaced) | `shim/gow3gpu.cpp` | `GOW3_LOCK_GUEST_MEMORY_API=1` restores the lock |
| `is_mapped` uses the cached fast-range check instead of a lease | `runtime/core/gow3gpu_runtime_adapter.c` | none |
| 8-entry per-thread extent cache (was 1) | `runtime/core/recomp_guest_memory.c` | none |
| Per-thread shader-code cache, re-validated by comparing the guest bytes through the binary info | `amdgpu/regs_shader.h` `GetParams` | `GOW3_SHADER_CODE_CACHE=0` |
| Counters: `regmem_wait_ns`, `regmem_waits`, `submit_lock_ns` in `GOW3_BENCHMARK_CSV`; `RegMem wait` lines under `GOW3_PERF_DIAG=1` | `liverpool.cpp`, `gnmdriver.cpp`, `driver.cpp` | diagnostic only |

The first shader-cache version re-validated only the 32-byte binary info and shrank the code
vector to its stated length. That run aborted once in an async SPIR-V compile
(`EmitGetAttribute` → `Sirit::OpLoad`). The committed version compares every guest byte from
the shader base through the binary info, so the reused code is the same as a fresh read. It ran
without that abort. The cause of the first abort is not proven.

## Results

All windows below are 5 s `Frame stats` windows of the same gameplay segment with
2,500–3,300 draws per frame. "GPU ticks" is the wall-time share that CPU threads, mostly the
command thread at the one-frame-ahead presenter limit, spend waiting for GPU completion.

| Build | FPS per window | GPU ticks | Bound by |
|---|---|---:|---|
| Before (`e886769` runtime; runs `…173816…`, `…174255…`) | 6.4–9.5 at 2,600–2,960 draws | 0–15% | CPU-side threads |
| After (all changes) | 9.6–11.1 at 2,500–3,300 draws | 84–89% | GPU |

With `GOW3_LOCK_GUEST_MEMORY_API=1 GOW3_SHADER_CODE_CACHE=0` (the two toggles off, the
lock-free read and extent cache still in), the command thread waits on the GPU 69–81% of the
time at 10–11 FPS. The fast read and the extent cache removed most of the contention. The
shader cache adds CPU headroom; the frame rate no longer shows CPU-side savings now that the
GPU limits it.

Metal attach trace, 5.7 s of gameplay after the changes (`mst_new.trace`):

| Quantity | Before (Spartan mst1) | After |
|---|---:|---:|
| Game GPU busy | 60% | 80% |
| Fragment stage union | 87% of busy | 90% of busy |
| Render encoders per frame | ~111 | ~107 |
| Compute/blit encoders per frame | ~169 | ~162 |

Other processes' (or unattributed) GPU work fills 625 ms of the remaining 1,160 ms of gaps,
in blocks of about 20 ms. Its owner is not identified.

720p scene rendering (`GOW3_DIAGNOSTIC_MSAA_SCALING=1 GOW3_DIAGNOSTIC_SCENE_SIZE=1280x720`,
55% of scene draws scaled) measures the same 100 ms median on the new build. The GPU time is
therefore not proportional to scene pixels either. Candidates are per-pass fixed costs: the
attachment load/store of each of the ~107 render encoders, KosmicKrisp's encoder splits at
barriers, compute work inside render passes, the 2048² shadow cascades, and the post chain.
This needs per-pass GPU attribution (`GOW3_METAL_PASS_LABELS=1` with label export), which is the
next step.

## Review of an external analysis (Argon)

Checked against the code and run logs:

- **Confirmed:**
  - `sceGnmSubmitDone` makes the next submit wait until the GPU thread has retired the
    frame (`submit_lock_ns`: about 86 ms of a 99 ms frame). This is a symptom of a slow GPU
    side, not an independent cost: removing it would let the guest run ahead, but throughput is
    still set by the GPU-side pipeline.
  - Guest-memory reads took global mutexes. Fixed above, though the waits were mostly the
    VM locks in `query_region` and fast-range misses, not only the API-table mutex.
  - `Rasterizer::InvalidateMemory` drains the draw pipe before checking `IsMapped`, and file reads
    notify CPU writes before and after `pread`. Not yet changed.
  - `BeginRendering` always stores, and loads unless clearing.
  - VBlank and flip `tsc` values in `ps4_videoout.c` use raw `cntvct_el0` (24 MHz), while
    `sceKernelReadTsc` returns 2.13 GHz ticks. Not yet changed; the effect on cutscene timing is
    unmeasured.
  - `sceGnmSubmitAndFlipCommandBuffers` in the runtime submits and then flips from the CPU,
    and `sceGnmInsertWaitFlipDone` emits a wait that is always satisfied. Whether God of War III
    calls these at runtime is not yet established.
- **Not supported:**
  - "AJM run flags are swapped." The definitions match the PS4 job layout used by shadPS4. The
    logged truncations are `flags=0x800000001001`: run flag 2 (multiple frames) plus the
    stream sideband, with a 24-byte output buffer that holds exactly the result (8) and stream
    (16) sidebands, so the optional multi-frame sideband has no room. Swapping the bits would
    decode one frame per job and write codec info over the guest's buffer.
  - The 12-byte stream sideband size in that analysis: it is 16 bytes in this runtime.
  - In gameplay the command thread is not mostly spinning in `WAIT_REG_MEM` (see above).

## Open

- The pre-existing guest crash `FATAL: Unresolved indirect jump/call ... (from
  RIP=0x1b50150, return addr=0x1b6e876)` after `my_decode_thread` exits now appears
  at 229 s, because the faster runs reach that cutscene sooner. It is the next correctness
  blocker for longer benchmarks.
- `DrawPipe::Run` and `Semaphore::Wait` spin while waiting; they burn a core each. Harmless
  for throughput on the 8-core M2 now, but worth turning into blocking waits for power and heat.
