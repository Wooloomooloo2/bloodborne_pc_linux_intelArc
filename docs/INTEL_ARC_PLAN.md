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

### Baseline (fixed build, system Mesa 26.2.5, Central Yharnam save, standing still, uncapped)

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
