"""Time real encrypted Pouch layouts; keep all generated state inside build/."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--smoke", action="store_true",
                        help="1501 samples, three-slot rings; correctness only, not performance evidence")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    root = args.root.resolve()
    if not root.is_relative_to(repo / "build"):
        parser.error("--root must be inside this repository's build directory")
    root.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    for days in (180, 365):
        expected = None
        results = []
        for layout in ("sample", "partition", "shard"):
            for schedule in ("boundary", "eager"):
                minutes = 1501 if args.smoke else days * 1440
                with tempfile.TemporaryDirectory(prefix="metrics-", dir=root) as work:
                    command = [str(binary), str(repo / "bench/metrics_storage.lua"),
                               work, str(days), layout, schedule, str(minutes),
                               "smoke" if args.smoke else "full"]
                    marks = {}
                    result = None
                    with subprocess.Popen(command, cwd=work, stdout=subprocess.PIPE,
                                          text=True) as process:
                        try:
                            for line in process.stdout:
                                line = line.strip()
                                if line in ("WRITE", "READ", "DONE"):
                                    marks[line] = time.monotonic()
                                elif line:
                                    result = json.loads(line)
                            if process.wait() != 0:
                                raise RuntimeError(f"benchmark failed: {layout}/{schedule}")
                        finally:
                            if process.poll() is None:
                                process.terminate()
                                try:
                                    process.wait(timeout=10)
                                except subprocess.TimeoutExpired:
                                    process.kill()
                                    process.wait()
                    assert result is not None and set(marks) == {"WRITE", "READ", "DONE"}
                    assert (Path(work) / "key").is_file(), "encrypted Pouch key was not created"
                    signature = tuple(result[k] for k in ("source_samples", "retained", "digest"))
                    if expected is None:
                        expected = signature
                    assert signature == expected, "layouts/schedules retained different samples"
                    result.update(days=days, layout=layout, schedule=schedule,
                                  write_seconds=marks["READ"] - marks["WRITE"],
                                  read_seconds=marks["DONE"] - marks["READ"])
                    results.append(result)
                    print(json.dumps(result, sort_keys=True), flush=True)
        if not args.smoke:
            winner = min(results, key=lambda r: r["write_seconds"] + r["read_seconds"])
            print(f"fastest measured write+read for {days}d: {winner['layout']}/{winner['schedule']}")
    if args.smoke:
        print("12 encrypted-Pouch scenarios passed; timing is smoke-only, not a storage recommendation")


if __name__ == "__main__":
    main()
