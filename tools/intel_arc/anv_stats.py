"""anv_stats.py CACHE_DIR ANV_LOG [PROFILE_LOG]: joins ANV's per-shader statistics (INTEL_DEBUG,
src_hash = first 8 bytes of BLAKE3(SPIR-V), little-endian) with bbport's cached SPIR-V
(<pgm_hash>_<perm>.spv) and, given a BB_GPU_PROFILE log, ranks the shaders of the heaviest passes.
Also counts NoContraction decorations per stage (BB_SPIRV_PRECISE check)."""
import collections
import multiprocessing
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import blake3_ref  # noqa: E402

MODELS = {0: "vs", 1: "tcs", 2: "tes", 3: "gs", 4: "fs", 5: "cs"}


def spirv_info(path: Path):
    data = path.read_bytes()
    words = struct.unpack(f"<{len(data) // 4}I", data[: len(data) // 4 * 4])
    stage, nocontraction = "?", 0
    i = 5
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        if wc == 0:
            break
        if op == 15 and stage == "?":  # OpEntryPoint
            stage = MODELS.get(words[i + 1], "?")
        elif op == 71 and wc >= 3 and words[i + 2] == 42:  # OpDecorate NoContraction
            nocontraction += 1
        i += wc
    src_hash = struct.unpack("<Q", blake3_ref.blake3(data, 8))[0]
    return path.name, stage, nocontraction, src_hash


STAT = re.compile(r"SIMD(\d+) shader: (\d+) instructions\. (\d+) loops\. (\d+) cycles\. (\d+):(\d+) spills:fills, (\d+) sends")
HEAD = re.compile(r"Native code for unnamed (\w+) shader.*?src_hash 0x([0-9a-f]+)")


def anv_stats(log: Path):
    stats = collections.defaultdict(list)
    current = None
    for line in log.read_text(errors="replace").splitlines():
        m = HEAD.search(line)
        if m:
            current = int(m.group(2), 16)
            continue
        m = STAT.search(line)
        if m and current is not None:
            simd, inst, loops, cycles, spills, fills, sends = (int(x) for x in m.groups())
            stats[current].append(dict(simd=simd, inst=inst, cycles=cycles, spills=spills,
                                       fills=fills, sends=sends))
    return stats


def hot_shaders(profile_log: Path):
    """(ms/frame, label) of the last GPU profile block."""
    lines = profile_log.read_text(errors="replace").splitlines()
    start = max(i for i, l in enumerate(lines) if l.startswith("GPU profile:"))
    out = []
    for l in lines[start + 1:]:
        m = re.match(r"\s+([\d.]+) ms/frame\s+[\d.]+/frame\s+(.*)", l)
        if not m:
            break
        out.append((float(m.group(1)), m.group(2)))
    return out


def main():
    cache, log = Path(sys.argv[1]), Path(sys.argv[2])
    files = sorted(cache.glob("*.spv"))
    with multiprocessing.Pool() as pool:
        infos = pool.map(spirv_info, files)
    stats = anv_stats(log)
    by_stage = collections.defaultdict(lambda: [0, 0])
    for name, stage, nc, _ in infos:
        by_stage[stage][0] += 1
        by_stage[stage][1] += nc > 0
    print("SPIR-V files by stage (count, with NoContraction):", dict(by_stage))
    matched = sum(1 for _, _, _, h in infos if h in stats)
    print(f"matched {matched} of {len(infos)} cached modules to {len(stats)} ANV compiles")
    by_pgm = collections.defaultdict(list)
    for name, stage, nc, h in infos:
        by_pgm[name.split("_")[0][2:].lstrip("0") or "0"].append((name, stage, h))

    def describe(pgm):
        rows = []
        for name, stage, h in by_pgm.get(pgm.lstrip("0") or "0", []):
            for s in stats.get(h, []):
                rows.append(f"{name} {stage} SIMD{s['simd']}: {s['inst']} inst, {s['cycles']} cycles, "
                            f"{s['spills']}:{s['fills']} spills:fills, {s['sends']} sends")
        return rows or ["(no ANV stats matched)"]

    if len(sys.argv) > 3:
        for ms, label in hot_shaders(Path(sys.argv[3]))[:16]:
            print(f"\n{ms:6.3f} ms/frame  {label[:150]}")
            for kind, pgm in re.findall(r"\b(vs|ps|cs) ([0-9a-f]{8,16})", label):
                if kind == "vs":
                    continue
                for row in describe(pgm):
                    print("     ", row)
    # Spills anywhere
    spilled = [(name, stage, s) for name, stage, _, h in infos for s in stats.get(h, []) if s["spills"]]
    print(f"\nmodules with spills: {len(set(n for n, _, _ in spilled))}")
    for name, stage, s in sorted(spilled, key=lambda x: -x[2]["spills"])[:15]:
        print(f"  {name} {stage} SIMD{s['simd']}: {s['spills']}:{s['fills']} spills:fills, {s['inst']} inst")
    simd = collections.Counter((stage, s["simd"]) for name, stage, _, h in infos for s in stats.get(h, []))
    print("\nSIMD widths compiled (stage, width): count", dict(sorted(simd.items())))


if __name__ == "__main__":
    main()
