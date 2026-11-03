# Intel Arc measurement tools (2026-11-03)

Used for `docs/INTEL_ARC_PLAN.md`. Not part of the build.

| File | What it does |
|---|---|
| `abench.sh NAME UPSCALER PRESET [ENV=VALUE...]` | One unattended run: a fresh copy of the save in `~/Games/bbport-dev/user`, Continue, wait for the level, settle 15 s, measure `HOLD` s (60) standing still. Per-frame CSV, GPU clock / throttle / temperature each second, one presented frame dumped 20 s in. Finds the game by its save folder. `KEEP_USER=1` keeps the save copy (its shader cache), `LOG_FILTER=regex` filters the log (for `INTEL_DEBUG`). `RESULTS=dir` for the output. |
| `abench_walk.sh` | The same, `WALK=1` walks and turns during the measurement (`tools/soak_walk.sh`'s pattern). |
| `summarize.py RESULTS NAME...` | One line per run: FPS, median, p99, frames over 50 ms, mean GPU clock, max temperature, throttle reasons. |
| `rawimg.py png RAW OUT.png` / `rawimg.py diff A.raw B.raw` | The presented-frame dumps (RGBA8 1680x1050) as PNG, and their difference. |
| `memprobe.c` | Repeats the port's guest-memory startup checks (host-pointer import, dma-buf export and its CPU read speed) outside the game. |
| `ccs_probe.c` | Creates the port's kinds of render targets and prints ANV's memory requirements. |
| `compile_cs.c` | Compiles one compute SPIR-V on the Intel GPU (to see `INTEL_DEBUG` output for a known module). |
| `blake3_ref.py` | BLAKE3 (ANV's `src_hash` is the first 8 bytes, little-endian, of BLAKE3 of the SPIR-V). |
| `anv_stats.py CACHE_DIR ANV_LOG [PROFILE_LOG]` | Joins `INTEL_DEBUG` shader statistics with the port's cached SPIR-V and a `BB_GPU_PROFILE` block. |
| `measure_report.py MEASURE.csv [CACHE_DIR]` | Summarizes an `INTEL_MEASURE` CSV (unused so far: `INTEL_MEASURE` crashes ANV on this game). |
| `fedora-shims/` | Building on Fedora 44: `CMAKE_PREFIX_PATH=$PWD/tools/intel_arc/fedora-shims` (Zydis has no CMake config there). Also needed: `CXXFLAGS="-isystem <Vulkan-Headers v1.4.357>/include -isystem <dir with magic_enum/*.hpp symlinks to /usr/include/magic_enum*.hpp>"`. |

C probes: `gcc -O1 -o memprobe memprobe.c -lvulkan`.
The scripts hard-code this checkout's path and `~/Games/CUSA03173`.
