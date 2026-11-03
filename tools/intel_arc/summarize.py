"""summarize.py RESULTS NAME...: one line per run from its measured frames and sensors."""
import csv
import statistics
import sys
from pathlib import Path


def summarize(results: Path, name: str) -> str:
    start, end = (int(x) for x in (results / f"{name}.range").read_text().split())
    with open(results / f"{name}.frames.csv") as f:
        rows = list(csv.DictReader(f))
    # range counts file lines (header included): data rows start - 1 .. end - 1
    frames = [float(r["frame_ms"]) for r in rows[max(0, start - 1):end - 1] if float(r["frame_ms"]) > 0]
    if not frames:
        return f"{name:24s} no frames measured"
    total = sum(frames)
    fps = 1000.0 * len(frames) / total
    s = sorted(frames)
    p99 = s[min(len(s) - 1, int(0.99 * len(s)))]
    over50 = sum(1 for x in frames if x > 50.0)
    act, temp, thr = [], [], set()
    sp = results / f"{name}.sensors.csv"
    if sp.exists():
        with open(sp) as f:
            for r in csv.DictReader(f):
                act.append(int(r["act_mhz"]))
                temp.append(int(r["temp_c"]))
                if r["throttle"] not in ("none", ""):
                    thr.add(r["throttle"])
    clock = f"{statistics.mean(act):5.0f} MHz" if act else "   ?"
    t = f"{max(temp)}C" if temp else "?"
    return (f"{name:24s} {fps:6.1f} FPS  median {statistics.median(frames):5.1f} ms  "
            f"p99 {p99:5.1f} ms  >50ms {over50:3d}  frames {len(frames):5d}  "
            f"GPU {clock}  max {t}  throttle {','.join(sorted(thr)) or 'none'}")


if __name__ == "__main__":
    results = Path(sys.argv[1])
    for name in sys.argv[2:]:
        print(summarize(results, name))
