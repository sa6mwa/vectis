#!/usr/bin/env python3
"""Verify local ELF runtime selection without a runtime launcher."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


def output(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs)


def check_libraries(trace, libraries, roots):
    assert "not found" not in trace, trace
    checked = set()
    for name, path in re.findall(r"(\S+) => (\S+)", trace):
        if name in libraries:
            assert any(os.path.samefile(path, candidate) for candidate in libraries[name]), (name, path)
        else:
            assert any(root in Path(path).resolve().parents for root in roots), (name, path)
        checked.add(name)
    assert "libc.so.6" in checked, trace


def main():
    sdk = sys.argv[1] == "--sdk"
    build = Path(sys.argv[2] if sdk else sys.argv[1]).resolve()
    values = dict(re.findall(r"^([^:#\n]+):[^=\n]*=(.*)$",
                            (build / "CMakeCache.txt").read_text(), re.M))
    root = Path(__file__).resolve().parent.parent
    sysroot = Path(values["CMAKE_SYSROOT"]).resolve()
    readelf = values["CMAKE_READELF"]
    loader = sysroot / "lib/ld-linux-x86-64.so.2"
    assert loader.is_file(), f"missing pinned loader: {loader}"
    discovery = output(str(root / "scripts/cpkt-toolchains.sh"), "discover", "x86_64-linux-gnu")
    pinned = dict(line.split("=", 1) for line in discovery.splitlines() if "=" in line)
    assert sysroot == Path(pinned["sysroot"]).resolve(), "configured runtime differs from pin"
    env = {k: v for k, v in os.environ.items() if not k.startswith("LD_")}
    allowed_roots = [build]
    if sdk:
        allowed_roots += [Path(p).resolve() for p in values.get("CMAKE_PREFIX_PATH", "").split(";") if p]
    libraries = {}
    for directory in (sysroot / "lib", sysroot / "usr/lib", sysroot.parent / "lib64"):
        for path in directory.glob("*.so*"):
            if path.is_file():
                libraries.setdefault(path.name, set()).add(path.resolve())
    binaries = (build / "test-runtime/targets.txt").read_text().splitlines()
    if sdk:
        binaries += sys.argv[3:]
    assert binaries, "no local executable runtime targets"
    for filename in binaries:
        binary = Path(filename)
        assert binary.is_file(), f"build all targets before verification: {binary}"
        assert f"[Requesting program interpreter: {loader}]" in output(readelf, "-l", str(binary)), binary
        trace = output(str(binary), env=dict(env, LD_TRACE_LOADED_OBJECTS="1"))
        try:
            check_libraries(trace, libraries, allowed_roots)
        except AssertionError as error:
            raise AssertionError(f"{binary}: {error}") from error
    probe = build / "tests/vectis_runtime_probe"
    if probe.exists():
        assert output(str(probe), env=env).strip() == "self-exec OK"
    with tempfile.TemporaryDirectory(dir=build, prefix="runtime-negative-") as directory:
        binary = str(Path(directory) / "probe")
        subprocess.run([values["CMAKE_C_COMPILER"],
                        str(root / "tests/helpers/vectis_runtime_probe.c"),
                        "-o", binary, f"-Wl,--dynamic-linker={loader}",
                        "-Wl,-z,nodefaultlib", f"-Wl,-rpath,{directory}"], check=True)
        result = subprocess.run([binary], env=dict(env, LD_TRACE_LOADED_OBJECTS="1"),
                                capture_output=True, text=True)
        # A missing RPATH may still find a host multiarch cache entry. Prove the
        # mapping gate distinguishes it from the configured pinned libc.
        try:
            check_libraries(result.stdout, libraries, allowed_roots)
        except AssertionError:
            pass
        else:
            raise AssertionError("missing-runtime fixture escaped the mapping gate")
        exe_flags = shlex.split(values.get("CMAKE_EXE_LINKER_FLAGS_DEBUG", ""))
        shared_flags = shlex.split(values.get("CMAKE_SHARED_LINKER_FLAGS_DEBUG", ""))
        if any("fsanitize=" in flag for flag in exe_flags):
            source = str(root / "tests/helpers/vectis_runtime_probe.c")
            shared = str(Path(directory) / "libruntime-probe.so")
            subprocess.run([values["CMAKE_C_COMPILER"], source, "-fPIC", "-shared",
                            *shared_flags, "-Wl,-soname,libruntime-probe.so",
                            "-o", shared], check=True)
            metadata = output(readelf, "-d", shared)
            assert "libasan.so" not in metadata and "libubsan.so" not in metadata, metadata
            subprocess.run([values["CMAKE_C_COMPILER"], source, *exe_flags,
                            f"-L{directory}", "-Wl,--no-as-needed", "-lruntime-probe",
                            f"-Wl,--dynamic-linker={loader}", "-Wl,--disable-new-dtags",
                            f"-Wl,-rpath,{directory}:{sysroot}/lib:{sysroot}/usr/lib:{sysroot.parent}/lib64",
                            "-o", binary], check=True)
            trace = output(binary, env=dict(env, LD_TRACE_LOADED_OBJECTS="1"))
            check_libraries(trace, libraries, allowed_roots)
            assert output(binary, env=env).strip() == "self-exec OK"
    for library in build.glob("libvectis.so*"):
        metadata = output(readelf, "-d", str(library))
        assert str(sysroot) not in metadata and "NODEFLIB" not in metadata, library
    if sdk:
        pass
    elif values.get("CMAKE_BUILD_TYPE") == "Release":
        assert "INTERP" not in output(readelf, "-l", str(build / "vectis"))
        assert "NEEDED" not in output(readelf, "-d", str(build / "vectis"))
    else:
        result = subprocess.run(["cmake", f"-DVECTIS_BINARY_DIR={build}",
                                 f"-DVECTIS_ROOT={root}", "-P",
                                 str(root / "cmake/package_archive.cmake")],
                                capture_output=True, text=True)
        assert result.returncode != 0 and "requires a Release build" in result.stderr, result
    print(f"runtime contract passed: {len(binaries)} executables; pinned libraries; missing-runtime detection")


if __name__ == "__main__":
    main()
