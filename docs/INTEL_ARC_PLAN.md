# Intel Arc (integrated Xe) optimisation plan

Goal: make bbport the best way to play Bloodborne on Intel Arc laptops, the way the macOS port is
on Apple silicon. Driver-level and port-level GPU work first; upscaling is the fallback, not the
plan.

Target, in order: **a locked 60 FPS at the laptop's native 1050p class with no upscaler**, no
periodic hitches; then 60 at higher output resolutions with FSR 3.1 Quality.

## Test machine

| | |
|---|---|
| Laptop GPU | Intel Arc B390 (Panther Lake, Xe3, integrated), PCI 8086:b082, kernel driver `xe` |
| CPU | Core Ultra X7 358H: 4 P-cores (cpu 0-3, 4.7 GHz), 8 E-cores (4-11, 3.7 GHz), 4 LP E-cores (12-15, 3.3 GHz) |
| Memory | 30 GiB shared (the GPU sees one 23 GiB device-local heap) |
| Display | 2880x1920 built-in, 60 Hz |
| Driver | Fedora 44 Mesa 26.2.5 (ANV). Note: the release AppImage bundles its own Mesa (26.2.4) |

## Where we are (2026-11-01)

**0.7 did not start on this GPU.** `STOP: the GPU driver cannot use the game's memory in place`.
Cause (port, not driver): `FindHostType` (`gpu/shim/bbport_guest_memory_chunks.cpp`) only took
host-visible memory types that are *not* device-local; ANV on an integrated GPU exposes every
type as device-local, and allows host-pointer imports only into those, so the port found no type
and reported `ErrorFeatureNotPresent` itself. Fixed locally: device-local host-visible types are
the fallback when nothing else is allowed (other GPUs unchanged). The dma-buf path stays rejected
on purpose: ANV's dma-buf mmap is uncached here (CPU reads ~50 MB/s).

Standalone proof: a small Vulkan probe that replays the port's checks (host import of memfd,
anonymous and malloc'd memory works into types 1 and 2; dma-buf export works but maps uncached).

### Baseline (fixed build, system Mesa 26.2.5, the save's spot: an indoor cathedral hall, standing still, uncapped)

`HOLD=45`, `tools/bench.sh` on a copy of the save (`BB_USER_DIR`), all effects on except SSR.

| Upscaler | Scene | FPS |
|---|---|---|
| FSR 3.1 Native AA (what the launcher picked: FSR 4 → fell back to 3.1) | 1680x1048 | ~40 |
| none | 1680x1048 | ~45 |
| FSR 3.1 Quality (x1.5) | ~1120x700 | ~65 |
| FSR 3.1 Performance (x2) | 840x524 | ~74 |

What these say:

- **GPU-bound.** The game waits ~26 ms/frame for the previous frame on the GPU; the port's GPU
  thread is 55-70 % idle; no write faults, no host copies, no VRAM mirrors (unified memory mode
  works as designed).
- **A large fixed cost.** Halving the pixels again (Quality → Performance) buys only ~9 FPS:
  much of the frame does not scale with the scene resolution (shadow maps, compute, the
  upscaler and UI at output size). That fixed cost is the main target.
- **FSR 3.1 costs ~2.5 ms at 1050p** (45 → 40 FPS at the same scene size): heavy for this
  resolution.
- **A 60-80 ms hitch in every 5 s window**, standing still, with no shader compiles. Separate
  from the average; players feel it.

## Workstreams

Each change is A/B tested with the same harness (below) and a screenshot comparison; numbers go
into a findings section at the end of this file.

### 0. Measurement harness (first, everything depends on it)

- `tools/bench.sh` + a matrix script: Continue from a copy of the save, wait for the level, stand
  45 s, log + per-frame CSV. Unattended; 5 configurations in ~10 minutes.
- Add a **walk** (the macOS port's recorded route, `BB_PAD_RECORD` / pad playback) so traversal,
  streaming and the hitch are covered, not only standing still.
- Record GPU clock and power during each run (`intel_gpu_top -J`), on mains, fixed power profile:
  a laptop iGPU throttles, and a 2 ms win is meaningless if the clock moved.
- Run against any Mesa build: `VK_DRIVER_FILES=<mesa build>/share/vulkan/icd.d/intel_icd.x86_64.json`
  (system driver, a local Mesa tree, or the AppImage's bundled one), so driver patches are measured
  with the same game build.

### 1. Per-pass GPU profile on Xe3

- The port's profiler (`BB_GPU_PROFILE=1`, `BB_GPU_PROFILE_TOP=n`): GPU time per labelled pass.
- ANV's own view: `INTEL_MEASURE=...` (per render pass / dispatch timings from the driver),
  to cross-check the port's timestamps and see what the driver adds between passes
  (resolves, aux/compression ops, cache flushes, stalls).
- Output: the 10 heaviest passes, their shaders (guest hash), and how they scale with resolution.
  This decides the order of everything below.

### 2. Port: "precise" float math only where it is needed (from the macOS port)

The SPIR-V backend marks every float add, multiply and FMA in every stage `NoContraction`
(`gpu/shader/backend/spirv/emit_spirv_floating_point.cpp`). That stops the Intel compiler from
fusing multiply+add into `MAD` and from reassociating. On the Mac, keeping it only in the vertex
stages (positions must match bit for bit across depth prepass, G-buffer and shadow passes) took
the city respawn spot from 39 to 60 FPS (GPU 19.6 → 12.1 ms/frame), screenshots unchanged.

- Port the macOS change: `NoContraction` in vertex/tessellation/geometry stages only;
  `BB_SPIRV_PRECISE=all` restores the old code; bump the shader/pipeline cache version.
- Measure: the matrix, plus per-pass profile; screenshots (`tools/shot.sh`) for z-fighting or
  shimmer in the depth-tested passes.
- Expected: smaller than on Metal (Metal made each precise op an unoptimisable call), but the hot
  fragment and compute shaders should get cheaper.

### 3. Port: robustness costs

`robustBufferAccess`, `robustBufferAccess2` and `robustImageAccess2` are on for every pipeline.
On Intel these turn into bounds checks on buffer and image accesses in the shaders.

- Measure what they cost: `BB_ROBUSTNESS=0` (already in the port, null descriptors stay).
- If significant: per-pipeline robustness (`VK_EXT_pipeline_robustness`) only for the shaders that
  need it (the macOS port found the game reading through empty resource tables in a few places:
  those keep it), instead of device-wide.

### 4. Shader compiler: what ANV does with the hottest shaders (driver level)

For the heaviest passes from step 1:

- Dump with `INTEL_DEBUG` (shader stats/assembly): SIMD width chosen (8/16/32), register spills
  and fills, instruction and send counts, and how they change with the step 2 SPIR-V.
- Map guest shader hash → SPIR-V (`<user>/cache/CUSA03173/*.spv`) → compile standalone, so a
  shader can be iterated on without running the game.
- Classify each: **(a) the port's SPIR-V is awkward for Intel** (fix in the translator: e.g.
  64-bit address math, GCN-style scalar/vector patterns, conversions) or **(b) ANV/the Intel
  compiler could do better** (spills at SIMD16 that a better schedule avoids, missed MAD fusion,
  poor handling of a pattern the translator emits constantly) → Mesa work, with the shader as the
  reproducer.

### 5. Driver level: memory and image paths on an integrated Xe

Candidates to check in ANV once step 1 shows where the time goes:

- **Image compression (CCS/aux) on Xe3** for the game's render targets: are they compressed,
  are there resolves between passes, do layout transitions the port issues force ambiguous
  resolves?
- **Host-pointer imported memory** (the whole game memory is bound in place through it): which
  PAT/cache mode the `xe` kernel driver gives userptr binds, and whether GPU reads of it are as
  fast as ANV's own allocations.
- **dma-buf mmap is uncached** (~50 MB/s CPU reads) on ANV/xe: not needed by the port any more,
  but worth understanding for other emulators that map exported memory.
- Barriers and flushes the port issues per pass (`INTEL_MEASURE` shows the gaps).

### 6. FSR 3.1 on ANV

~2.5 ms at 1050p. Profile its passes; the port already rewrote FSR 4's last passes for RDNA3's
LDS (3.5x faster); similar treatment for Xe if one pass dominates. Also check whether FSR 4 INT8
is usable on Xe3 at all (its assets are not in a source build: `tools/fetch_fsr4_assets.sh`).

### 7. The periodic hitch

60-80 ms once in each 5 s window, standing still. Find it before tuning averages:

- Per-frame CSV (`BB_FRAME_LOG`): which frame, and what the GPU thread and guest threads were
  doing (`Stall:` lines already print a breakdown).
- Suspects: the texture collector or a periodic memory task, the frame-stats timer itself (check
  with `BB_FRAME_STATS=0`, `BB_FPS_PRINT=1`), a guest thread landing on an LP E-core.
- CPU placement on this hybrid CPU: game threads on P/E cores vs LP E-cores (cpu 12-15).

### 8. Upstream

Send to deadinside28/bloodborne_pc as separate PRs, each with measurements from this machine:

1. The integrated-Intel memory-type fix (makes 0.7 start on every integrated Arc/Xe).
2. Building on Fedora (and other non-Nix distros): Zydis without a CMake config (pkg-config
   fallback), magic_enum's flat header layout (use the bundled download), Vulkan headers older
   than 1.4.357 (`VK_VALVE_shader_mixed_float_dot_product`).
3. Each optimisation from steps 2-7 that holds up, with its A/B numbers.

Driver findings go to Mesa (ANV) with a standalone reproducer.

## Order

1. Harness + per-pass profile (steps 0-1).
2. `NoContraction` (step 2) and robustness (step 3): cheap, port-side, measurable at once.
3. The hitch (step 7).
4. The hottest shaders through the Intel compiler (step 4), then driver paths (step 5).
5. FSR 3.1 (step 6).
6. Upstream PRs as each piece is proven (step 8).

## Findings

### 2026-11-01: first per-pass GPU profile (native 1680x1048, no upscaler)

`BB_GPU_PROFILE=1`, same save and spot as the baseline: **23.15 ms of GPU time per frame**
(~40 FPS measured during the run, against ~45 without the profiler: its timestamps end render
passes, so read the rows as relative weights, the totals ~1-2 ms high), 133 labels. Heaviest:

| ms/frame | per frame | what |
|---|---|---|
| 2.02 | 41.7 dispatches | the game's buffer-copy compute shader `cs fefebf9f` (1024 groups each), translated and run on the GPU |
| 1.39 + 1.00 + 0.58 + 0.47 + 0.33 + 0.27 | ~6 passes | G-buffer passes, 6 targets (RGBA8 x4, R11G11B10F, RGBA16F) + D32S8 |
| 1.15 + 0.75 + 0.71 + 0.57 | 5 passes | lighting into two R11G11B10F targets, `vs 8747a367` with `ps 600f86b0`, `d92266b9`, `3fe9e964`, `89f9ee3e` |
| 0.67 + 0.58 + 0.54 + 0.44 + ... | many | full-screen RGBA16F passes (post-processing) |
| 0.43 | 4 passes | 840x524 RGBA8 (half-resolution effect) |
| 0.40 | 3 dispatches | `cs e2c27f0c`, 1x1x1 groups: a single workgroup taking 0.13 ms each |

Reading:

- **The copy shader is not Intel-specific**: upstream measured ~2.1 ms/frame for it on the RX 7800
  XT too (`docs/parallel_gpu.md`). It is the largest resolution-independent cost on every GPU;
  upstream has a batched path (`buffer_multi_copy.comp`, toggle bit 43, 2.1 → 1.6 ms). Worth
  measuring on Xe3, and worth a look at why 42 dispatches of 1024 groups cost 2 ms here: it
  writes through to the game's host-imported memory (step 5: the cache mode of userptr binds).
- **The rest is the scene at 1680x1048**: G-buffer ~4 ms, lighting ~3.2 ms, post-processing
  passes each 0.3-0.7 ms. These are the shaders for steps 2 and 4 (`NoContraction`, then
  `INTEL_DEBUG` on `ps 600f86b0` and the G-buffer pixel shaders first).
- `cs e2c27f0c` runs a **single workgroup** 3 times a frame at 0.13 ms each: serial work on a GPU
  built for width; check what it does and whether it can be widened or moved.
- Submission gaps are small (0.2 ms/frame in gaps over 0.25 ms): the CPU feeds the GPU fine; this
  is GPU execution time.

### 2026-11-02/03: measurement harness, first A/B results

Harness: one unattended run = a fresh copy of the save, Continue, wait for the level, settle 15 s,
measure 60 s standing still (or walking: forward, turn, back, turn), per-frame CSV
(`BB_FRAME_LOG`), and each second the GPU's actual clock, throttle reasons and package
temperature from sysfs (`/sys/class/drm/card1/device/tile0/gt0/freq0`). A/B runs interleaved
(A, B, A, B) so drift hits both. The game is found by its own save folder: `tools/restart.sh`
takes the newest `bb-probe`, which after a shader version change is the short-lived cache
rebuild child, and then cannot stop the real game.

Conditions for every run below: on mains, platform profile `balanced`, GPU at ~2.0-2.1 GHz
(of 2.5), throttle reason `pl4` throughout, package 71-77 °C. Native 1680x1048, no upscaler.

| Experiment | FPS (run 1 / run 2) | Median | p99 | Result |
|---|---|---|---|---|
| baseline | 44.2 / 44.0 | 22.4-22.6 ms | 41.3-41.6 ms | |
| `NoContraction` in vertex stages only (step 2) | 44.4 / 44.3 | 22.4 ms | 41.1-41.3 ms | no change (all: 44.1, none: 44.3) |
| robustness off, `BB_ROBUSTNESS=0` (step 3) | 43.9 / 44.1 | 22.5-22.6 ms | 41.4-41.5 ms | no change |
| compression off, `INTEL_DEBUG=noccs` | 29.8 / 30.0 | 33.2-33.3 ms | 61.3-61.4 ms | **-32 %**: compression is on and matters |
| copy shader HLE, `BB_COPY_SHADER_NATIVE=0` | 43.1 / 43.5 | 22.9-23.1 ms | **25.6-25.9 ms** | -1.5 % average, the 16-frame stutter gone |

- **`NoContraction`** (the macOS port's biggest win) does nothing measurable on ANV. Verified
  that the mode works: 0 of 270 fragment modules and 240 of 272 vertex modules carry it. The Mac
  gain came from how SPIRV-Cross/Metal compile precise math (an `optnone` call per op); ANV only
  loses MAD fusion, and these shaders are not ALU-bound. Kept as the default (it matches the Mac
  port and costs nothing); not worth an upstream PR on Intel numbers.
- **Robustness** costs nothing measurable here.
- **Compression (CCS) is active for the port's render targets and is worth a third of the frame
  rate.** The frame is memory-bandwidth-bound: an iGPU sharing LPDDR5X with the CPU. The
  image create flags the port uses (`MUTABLE_FORMAT | EXTENDED_USAGE`, storage, no format list)
  do not change ANV's memory requirements for these images (probe: same size, types 0-2, as a
  plain render target), and `noccs` shows compression is on today.
- **The default copy-shader path makes every 16th frame ~40 ms, followed by a ~6 ms one** (178 of
  ~2,650 frames, 16 frames apart; the pair averages the normal 23 ms: a late flip, not extra
  work). On screen that is a hitch about 2.7 times a second at 44 FPS. With the CPU-side (HLE)
  path the pattern disappears (remaining long frames: one per ~5 s, the stats printout itself).
  Same image (frame dumps: path-to-path difference 5.5-8.3/255 mean, within run-to-run
  noise of 3.7-9.3 in this animated scene). No recorder syncs in either path: not a CPU wait on
  the port's side. Upstream made the GPU path the default because the HLE copies once read a
  stale VRAM mirror; on an integrated GPU there are no VRAM mirrors.

### ANV shader statistics (INTEL_DEBUG, matched to bbport's shader IDs)

`INTEL_DEBUG=vs,fs,cs,gs,tcs,tes MESA_SHADER_CACHE_DISABLE=true`: ANV printed 866 compiles. Its
`src_hash` is the first 8 bytes (little-endian) of the BLAKE3 hash of the SPIR-V module, so each
cached module (`<user>/cache/CUSA03173/<pgm>_<perm>.spv`) can be matched: 463 of 604 matched (the
rest were not compiled in this short run).

- **No register spills in any shader.** ANV compiles this game's shaders cleanly.
- G-buffer pixel shaders (`ps 1c3953c6`, `bb10e440`, `52738b8a`; 6 targets): SIMD16 only,
  365-513 instructions, 22-32 sends. Lighting `ps 600f86b0`: SIMD16 and SIMD32, 349 instructions.
- Several of the heaviest passes are almost empty shaders: `ps 8650418f` (13 instructions,
  0.48 ms full-screen RGBA16F), `ps f7229091` (9 instructions, 0.44 ms over four 840x524 passes).
  Those passes are pure bandwidth, consistent with the `noccs` result.
- The copy shader `cs fefebf9f`: 26-175 instructions, SIMD32, yet 2.2 ms over 42 dispatches: the
  cost is around the dispatches (barriers, flushes), not in the shader.

Tools: `tools/intel_arc/` (the benchmark harness, the Vulkan probes, BLAKE3, `anv_stats.py`;
see its README).

### Mesa: `INTEL_MEASURE` crashes ANV on this workload (for a Mesa report)

`INTEL_MEASURE=draw,file=...,start=11900,count=60` (also with a `control=` FIFO, which captured
nothing) crashes the game when the capture window opens, at the first submission:

```
Host fault (signal 11) in libvulkan_intel.so+0x454d5, address 0x8
  _anv_measure_submit+53
  xe_queue_exec_locked+2228
  anv_queue_submit+1971
  vk_queue_submit_final+697
  vk_queue_submit+117
  <- Vulkan::Scheduler::QueueSubmit (bbport)
```

Mesa 26.2.5 (Fedora `mesa-vulkan-drivers-26.2.5-1.fc44`, build ID
`dccee9c55a9e1c73abbefdb76629ce471f91d7cf`), `xe` kernel driver, Arc B390. Same with one recording
thread and no submit thread (`BB_VK_RECORD_THREADS=1 BB_ASYNC_SUBMIT=0`), so not a threading
race in the app. A null structure read at +0x8 in `_anv_measure_submit`: likely a command buffer
whose measure state was never set up (one begun before the capture window, or reused; Mesa's own
warnings name reused command buffers and simultaneous-use secondaries as unsupported).

### 2026-11-03: copy path under FSR and walking; frames ahead; empty draws

Same harness and conditions. FPS of two interleaved runs each.

| Experiment | Setting | Baseline | Variant | p99 baseline → variant |
|---|---|---|---|---|
| copy shader HLE (`BB_COPY_SHADER_NATIVE=0`) | FSR 3.1 Quality, standing | 58.5 / 59.2 | 56.8 / 56.9 (-3.5 %) | 19.3-19.8 → 20.1-20.2 ms |
| copy shader HLE | native, **walking** | 45.7 / 46.1 | 45.0 / 45.0 (-1.8 %) | **40.2-40.4 → 25.7-25.9 ms** |
| `BB_FRAMES_AHEAD=2` | native | 44.3 / 44.0 | 44.1 / 43.7 | 41.4-42.0 → 41.5-41.8 ms (unchanged) |
| `BB_FRAMES_AHEAD=2` | FSR 3.1 Quality | 59.3 / 59.3 | 59.0 / 58.7 | unchanged |
| `BB_SKIP_EMPTY_DRAWS=1` | native | 44.2 / 44.0 | 44.1 / 44.2 | unchanged |
| `BB_SKIP_EMPTY_DRAWS=1` | FSR 3.1 Quality | 58.9 | 59.0 | unchanged |

- The 16-frame stutter shows at native (standing and walking), where the GPU is the limit; at
  FSR 3.1 Quality the default path has none, and HLE only costs. So HLE is not a default to
  switch to; the stutter needs its cause found.
- One more frame of slack for the command processor does not absorb it: the stall is in the GPU
  work itself, not in how far ahead the CPU runs.
- The game's depth-decompress draws are already skipped by default; the other empty draws cost
  nothing measurable here.
- FSR 3.1 Quality ran at ~59 FPS on 2026-11-02/03 against ~65 on 2026-11-01 (that day's run had
  no clock logging). All comparisons above are within one session, interleaved.

### 2026-11-03: barriers, CPU power, precise math under FSR

| Experiment | Setting | Baseline | Variant | Result |
|---|---|---|---|---|
| copy shader without its barriers (experiment bit 128, unsafe) | native | 44.3 / 44.5 | 44.3 / 44.3 | no change; same 16-frame p99 (41 ms) |
| `BB_SPIRV_PRECISE=all` (old shaders) vs `vertex` | FSR 3.1 Quality | vertex 58.8 / 59.0 | all 59.1 / 59.1 | no change: the 65 → 59 gap since 2026-11-01 is not this change |
| draw-preparation workers 4 → 0 (`BB_PREP_WORKERS=0`) | native | 44.4 / 44.3 | 44.2 / 44.4 | no change, GPU clock unchanged (~2,030-2,050 MHz) |
| lean CPU: no workers, one recording thread, no copy filter | native | 44.4 / 44.3 | 44.5 / 44.5 | no change, GPU clock unchanged |
| draw-preparation workers 4 → 0 | FSR 3.1 Quality | 59.0 / 59.1 | 59.1 / 59.5 | within noise |

- The 2.2 ms the profiler charges to the copy shader is not its barriers: the copies themselves
  (42 dispatches of 1024 workgroups through the game's memory in place), or GPU time attributed
  to it.
- The CPU's power does not starve this GPU, unlike the Steam Deck (upstream: workers off gave the
  Deck's GPU +33 MHz and +3 % FPS; `DefaultWorkerCount` applies that rule only below 12 hardware
  threads). Here the GPU runs at ~2.0 GHz of 2.5 with throttle reason `pl4` whatever the CPU
  does. Package limits: 35 W long term, 60 W short term; RAPL energy counters need root, so the
  CPU/GPU power split is not measured.
- The CPU has no SMT, so the port's core reservation (`BB_CORE_RESERVE`, SMT CPUs only) does not
  apply; threads spread over P, E and LP-E cores freely.

### 2026-11-03: which compression matters; the feedback loop layout

Native, standing, single runs between two baselines (44.2 / 44.3 FPS):

| ANV switch | FPS | Change | Reading |
|---|---|---|---|
| `INTEL_DEBUG=nohiz` (depth compression off) | 43.4 | -1.8 % | HiZ is on and helps a little |
| `INTEL_DEBUG=nofc` (fast clears off) | 43.7 | -1.2 % | fast clears happen and help a little |
| `INTEL_DEBUG=norbc` | 29.8 | **-33 %** | the colour render-target compression of Xe2+; same loss as `noccs` |

Every compression mechanism ANV has is in use for this game on Xe3, and colour compression is
worth a third of the frame. The bandwidth win is already collected: more needs less traffic in
the game's own passes.

`BB_FEEDBACK_LOOP=0` (new experiment switch: the targets the game samples while drawing into them
stay in the General layout instead of `ATTACHMENT_FEEDBACK_LOOP_OPTIMAL`): native 44.1 / 44.3
against 44.2 / 44.4, FSR 3.1 Quality 59.2 against 59.4. No change: ANV keeps compression in
either layout.

### 2026-11-03: the 16-frame stall is the game's, not presentation

Presentation ruled out (native, standing):

| Window / present mode | FPS | p99 |
|---|---|---|
| windowed, Mailbox (harness default) | 44.2 / 44.1 | 41.2-41.3 ms |
| full screen, Mailbox (the launcher's default; swapchain 2880x1919) | 43.3 / 43.6 | 41.5-41.9 ms |
| windowed, Immediate | 43.9 | 41.5 ms |
| full screen, Immediate | 42.7 | 42.5 ms |

The stall is there in every mode. Full screen is ~1.5 % slower here (the swapchain is the panel's
2880x1919, the window's 1694x1055).

The long frame is followed by a short one, the pair adding up to two normal frames; the long
frame carries no unusual work (dispatches, submissions, draws and stage-A CPU time are typical).
Its period is in **game frames**, not time:

| Run | FPS | Stall spacing | Spacing in time | Flip delay |
|---|---|---|---|---|
| native | 44.5 | 16 frames | 360 ms | 17.2 ms |
| native, compression off | 30.0 | 16 frames | 535 ms | 26.4 ms |
| native, walking | 45.8 | 16 frames | 350 ms | 16.4 ms |
| native, full screen Immediate | 43.0 | 16 frames | 374 ms | 17.8 ms |
| FSR 3.1 Quality | 58.6 | none (only the 5 s stats printout) | | |

So the game does something every 16 frames that makes the flip wait for the GPU (the delay is
~0.75 of a frame, so it scales with GPU load), and it only shows while the GPU is the limit. The
CPU-side copy path (HLE) hides it by changing when the copies' work lands.

Ruled out as the cause of the stall (native, standing; p99 41.3-41.7 ms in every run):
real occlusion queries (`BB_OCCLUSION_QUERIES=0`: 44.0 FPS), occlusion label order
(`BB_OCCLUSION_LABEL_ORDER=0`: 44.1), against 44.1 / 44.0. The game's waits per frame, averaged
over 5 s, look alike on both copy paths (GXWorker semaphores ~20-21 ms, "waited for the previous
frame" 18.6 vs 19.2 ms): the cause does not show in averages. It needs per-frame GPU timing,
which `INTEL_MEASURE` would give if it did not crash ANV (above), or a per-frame mode of
`BB_GPU_PROFILE`.

## Status at the end of the 2026-11-03 session

What the Arc B390 does with this game (native 1680x1048, no upscaler, ~44 FPS; FSR 3.1 Quality
~59 FPS):

- **The frame is limited by memory bandwidth on the GPU.** Colour compression (Xe2+ RBC) is on and
  worth a third of the frame; HiZ and fast clears are on and worth 1-2 % each. ANV compiles the
  game's shaders without spills.
- **Nothing on the port's side moved it**: `NoContraction` in vertex stages only, robustness off,
  the feedback loop layout, empty draws, frames ahead, the copy shader's barriers, CPU helper
  threads (the CPU's power does not starve this GPU). Each was measured twice, interleaved, with
  GPU clock logging.
- **A stutter at native**: every 16th game frame's flip slips by ~0.75 of a frame, then the next
  one goes out right behind it (2.7 times a second at 44 FPS). Not presentation, not occlusion
  queries; it disappears with the CPU-side copy path (at -1.5 to -3.5 % average) and at FSR 3.1
  Quality. Cause open.

Next, in order:

1. **The stutter**: per-frame GPU timing (a per-frame `BB_GPU_PROFILE` mode in the port, or
   `INTEL_MEASURE` once it works) on the long frame against its neighbours; then what the game's
   16-frame job is.
2. **Mesa**: the `INTEL_MEASURE` crash (`_anv_measure_submit`, above) — fixing it makes ANV's own
   per-event timing (resolves, clears, per-draw time by shader) available for everything else.
3. **Power**: the GPU runs at ~2.0 of 2.5 GHz with throttle reason `pl4` in the `balanced`
   platform profile. Worth one run in `performance` (needs root:
   `sudo tuned-adm profile throughput-performance` or the platform profile), with RAPL energy
   logged (also root), to see what the clock and frame rate do.
4. **Bandwidth in the game's passes**: the heaviest full-screen passes are near-empty shaders
   (`ps 8650418f`, 13 instructions, 0.48 ms; `ps f7229091`, 9 instructions, 0.44 ms over four
   half-resolution passes); look at what they read and write (formats, whether a pass could be
   merged or skipped on PC).
5. **For players now**: FSR 3.1 Quality with the 60 FPS cap gives ~59 FPS here. The first play
   session ran FSR 4 with the Native AA preset; without the FSR 4 assets (a source build) that
   silently became FSR 3.1 at Native AA, the most expensive setting (~40 FPS). The fallback
   should say so in the launcher and the overlay.

## 2026-11-04: Ubuntu 26.10 (kernel 7.3, Mesa 26.2.4), vertex fetch and the GPU TLB

Same laptop, now on Ubuntu 26.10: kernel 7.3.0-9 (`xe`), Mesa 26.2.4 (`mesa-vulkan-drivers
26.2.4-1ubuntu1`). The save has moved on: the bench spot is now **outdoors** (a cobbled street
in the Cathedral Ward area), heavier than the Fedora hall, so the numbers below are not
comparable with the Fedora tables. Per-pass costs that do not depend on the scene matched Fedora
(`ps 8650418f` 0.465 vs 0.48 ms, `ps f7229091` 0.433 vs 0.44 ms, the lighting passes within
0.1 ms): the driver and kernel are not slower.

### Start-up: four minutes before the first frame (fixed)

xe's userptr pinning on Linux 7.x goes through `drm_gpusvm_get_pages` → `hmm_range_fault`
(perf: 39 % `hmm_vma_handle_pte`, 28 % `vm_normal_page`, under `xe_vma_userptr_pin_pages`). For
memfd pages not yet faulted in, the walk restarts from the range start after each fault:
quadratic. Imports took 4.9 s (152 MiB), 55 s (512 MiB), 176 s (886 MiB). A probe repeats it
outside the game (untouched memfd 32 MiB 224 ms, 128 MiB 3.4 s; written or read pages under
10 ms). Fix: `MADV_POPULATE_WRITE` before each import (48 / 157 / 269 ms).

### The fixed cost is vertex fetch from 4 KiB pages

FSR 3.1 here: Native AA 25.5-28.5 FPS, Quality 37, Performance 39.5. Performance quarters the
pixels of Quality and gains 2 FPS: most of the frame does not scale with resolution.

- Per-draw profile (`BB_GPU_PROFILE_DRAWS=1`, Performance, 840x528): `vs 9da6aacb` + `ps bb10e440`
  124 draws 5.27 ms (42 µs each), `vs 7620368b` + `ps 8aba0ecd` 66 draws 3.17 ms (48 µs).
  The vertex shader is small (an instance 3x4 matrix, the view-projection, ~40 FMAs). Unchanged
  by robustness off, by `BB_SPIRV_PRECISE=none`, by `model_lod=1` and by the scene size: not ALU,
  not geometry count, not pixels.
- The game's vertex data is bound in place (1,351 MB bound per frame, 100 % in place) from the
  memfd. `/sys/kernel/mm/transparent_hugepage/shmem_enabled` is `never` (Ubuntu's default, also
  upstream's), so the memfd has 4 KiB pages and the runtime's `MADV_HUGEPAGE` on it does nothing.
- Probe (`readprobe`, 1 GiB, Arc B390): sequential reads 101-105 GB/s from driver memory, the
  memfd import and an anonymous THP import alike. **Scattered** 16-byte reads: driver memory
  42 ms, anonymous THP import 43 ms, **memfd import (4 KiB pages) 141 ms: 3.4x slower**. It is
  the GPU's TLB; xe maps 2 MiB pages when it gets them.
- `BB_VERTEX_READ_COPY=1` (upstream's experiment: the vertices each draw reads copied by the CPU
  into the stream buffer once per guest submission, ~92 MB a frame here) moves 98 % of the vertex
  data out of the memfd. The two draws above: 5.27 → 2.39 ms and 3.17 → 1.42 ms.

Interleaved A/B, 60 s standing, no profiler:

| FSR 3.1 | in place | vertex read copy | |
|---|---|---|---|
| Native AA | 28.4 / 28.5 FPS, >50 ms frames 198 / 220 | 33.6 / 33.7 FPS, 28 / 50 | +18 % |
| Quality (x1.5) | 35.7 / 37.0 FPS, p99 33-35 ms | 48.3 / 45.8 FPS, p99 23-26 ms | +30 % |

Frame dumps: base-to-copy difference 1.80/255 (Native AA) and 5.38/255 (Quality) against
base-to-base 1.76 and 5.24, i.e. noise. The GPU clock drops from ~2.12 to ~1.9 GHz with the
copy (the CPU's memcpy takes part of the package limit, `pl4`); still a net gain.

Walking (`WALK=1 abench_walk.sh`, 60 s): Native AA 27.6 → 30.9 FPS (p99 73 → 61 ms, >50 ms
frames 116 → 56), Quality 35.1 → 43.2 FPS (p99 42 → 29 ms). No new log warnings; the port's
stall reports dropped (684 → 156 windows at Native AA).

Now the default on Intel's integrated GPUs (`BB_VERTEX_READ_COPY=0` restores in place). Other
integrated GPUs (Steam Deck) unchanged until measured there.

**Open:** `shmem_enabled=advise` (root; persistent with the kernel argument
`transparent_hugepage_shmem=advise`) should give the memfd 2 MiB pages through the runtime's
existing `MADV_HUGEPAGE`, which would fix every in-place read (index data, the copy shader's
sources and destinations, constants) and not only vertex data, without the CPU copy. To measure
with and without the copy.
