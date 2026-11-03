"""measure_report.py MEASURE.csv [CACHE_DIR]: ANV's INTEL_MEASURE draw-level timings, per frame:
time by event type (draws, dispatches, resolves, clears, copies), and the heaviest fragment and
compute shaders, named by bbport's cached SPIR-V when CACHE_DIR is given (src_hash = BLAKE3)."""
import collections
import csv
import multiprocessing
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import blake3_ref  # noqa: E402


def module_hash(path: Path):
    return struct.unpack("<Q", blake3_ref.blake3(path.read_bytes(), 8))[0], path.stem


def main():
    rows = list(csv.DictReader(open(sys.argv[1])))
    frames = sorted({int(r["frame"]) for r in rows})
    nframes = max(1, len(frames))
    names = {}
    if len(sys.argv) > 2:
        with multiprocessing.Pool() as pool:
            names = dict(pool.map(module_hash, sorted(Path(sys.argv[2]).glob("*.spv"))))
    by_type = collections.defaultdict(lambda: [0.0, 0])
    by_fs = collections.defaultdict(lambda: [0.0, 0])
    by_cs = collections.defaultdict(lambda: [0.0, 0])
    idle = 0.0
    for r in rows:
        t = float(r["time_us"] or 0)
        idle += float(r["idle_us"] or 0)
        by_type[r["type"]][0] += t
        by_type[r["type"]][1] += int(r["count"] or 1)
        for col, table in (("fs", by_fs), ("cs", by_cs)):
            h = r.get(col, "") or ""
            if h and h != "0" and h != "0x0":
                table[int(h, 16)][0] += t
                table[int(h, 16)][1] += 1
    total = sum(v[0] for v in by_type.values())
    print(f"{nframes} frames, {total / nframes / 1000:.2f} ms/frame of events, "
          f"{idle / nframes / 1000:.2f} ms/frame idle between them")
    print("\nby event type (ms/frame, events/frame):")
    for k, (t, n) in sorted(by_type.items(), key=lambda kv: -kv[1][0]):
        print(f"  {k:28s} {t / nframes / 1000:7.3f}  {n / nframes:8.1f}")
    for title, table in (("fragment shaders", by_fs), ("compute shaders", by_cs)):
        print(f"\nheaviest {title} (ms/frame, events/frame):")
        for h, (t, n) in sorted(table.items(), key=lambda kv: -kv[1][0])[:15]:
            print(f"  {names.get(h, hex(h)):28s} {t / nframes / 1000:7.3f}  {n / nframes:8.1f}")


if __name__ == "__main__":
    main()
