#!/usr/bin/env python3
"""Prove runtime target enumeration follows reconfiguration, not stale files."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="runtime-reconfigure-", dir=sys.argv[1]) as directory:
    build = Path(directory)
    configure = ["cmake", "-S", str(root / "tests/runtime"), "-B", directory,
                 f"-DCMAKE_TOOLCHAIN_FILE={root}/cmake/toolchains/x86_64-linux-gnu.cmake",
                 "-DVECTIS_TARGET_ID=x86_64-linux-gnu",
                 f"-DCMAKE_PROJECT_INCLUDE={root}/cmake/test_runtime.cmake"]
    subprocess.run([*configure, "-DWITH_EXTRA=ON"], check=True)
    manifest = build / "test-runtime/targets.txt"
    assert len(manifest.read_text().splitlines()) == 2
    subprocess.run([*configure, "-DWITH_EXTRA=OFF"], check=True)
    assert manifest.read_text().splitlines() == [str(build / "runtime_kept")]
    subprocess.run(["cmake", "--build", directory], check=True)
    subprocess.run([sys.executable, str(root / "scripts/test_runtime_contract.py"),
                    "--sdk", directory], check=True)
    result = subprocess.check_output([str(build / "runtime_kept")], text=True)
    assert result.strip() == "self-exec OK", result
