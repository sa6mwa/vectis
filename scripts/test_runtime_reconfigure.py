#!/usr/bin/env python3
"""Prove runtime target enumeration follows reconfiguration, not stale files."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="runtime-unpinned-", dir=sys.argv[1]) as directory:
    env = {key: value for key, value in os.environ.items() if key != "CMAKE_TOOLCHAIN_FILE"}
    rejected = subprocess.run(["cmake", "-S", str(root), "-B", directory],
                              env=env, capture_output=True, text=True)
    assert rejected.returncode != 0 and "Linux builds require a pinned toolchain" in rejected.stderr, rejected
with tempfile.TemporaryDirectory(prefix="runtime-reconfigure-", dir=sys.argv[1]) as directory:
    build = Path(directory)
    configure = ["cmake", "-S", str(root / "tests/runtime"), "-B", directory,
                 f"-DCMAKE_TOOLCHAIN_FILE={root}/cmake/toolchains/x86_64-linux-gnu.cmake",
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
    # Library-only Release configurations still verify their test executables,
    # but must not require a CLI that the caller deliberately disabled.
    subprocess.run([*configure, "-DWITH_EXTRA=OFF", "-DCMAKE_BUILD_TYPE=Release",
                    "-DVECTIS_BUILD_BINARY=OFF"], check=True)
    subprocess.run(["cmake", "--build", directory], check=True)
    subprocess.run([sys.executable, str(root / "scripts/test_runtime_contract.py"), directory], check=True)
    rejected = subprocess.run([*configure, "-DVECTIS_TARGET_ID=x86_64-linux-musl"],
                              capture_output=True, text=True)
    assert rejected.returncode != 0 and "does not match" in rejected.stderr, rejected
