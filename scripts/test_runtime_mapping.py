#!/usr/bin/env python3
"""Exercise the runtime mapping gate with actual absolute DT_NEEDED entries."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_runtime_contract import check_libraries

build = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parent.parent
values = dict(re.findall(r"^([^:#\n]+):[^=\n]*=(.*)$",
                        (build / "CMakeCache.txt").read_text(), re.M))
sysroot = Path(values["CMAKE_SYSROOT"])
loader = sysroot / "lib/ld-linux-x86-64.so.2"
libraries = {name: {sysroot / "lib" / name}
             for name in ("libc.so.6", "ld-linux-x86-64.so.2")}
env = {key: value for key, value in os.environ.items() if not key.startswith("LD_")}


def reject(trace, roots):
    try:
        check_libraries(trace, libraries, roots)
    except AssertionError:
        return
    raise AssertionError(f"runtime gate accepted a forbidden mapping:\n{trace}")


with tempfile.TemporaryDirectory(prefix="runtime mapping-", dir=build) as directory:
    allowed = Path(directory) / "allowed"
    foreign = Path(directory) / "foreign"
    allowed.mkdir()
    foreign.mkdir()
    library = foreign / "libforeign.so"
    binary = allowed / "probe"
    source = root / "tests/helpers/vectis_runtime_probe.c"
    for named in (False, True):
        # Without SONAME the linker records the absolute filename in DT_NEEDED.
        soname = ["-Wl,-soname,libforeign.so"] if named else []
        subprocess.run([values["CMAKE_C_COMPILER"], str(source), "-shared", "-fPIC",
                        *soname, "-o", str(library)], check=True)
        subprocess.run([values["CMAKE_C_COMPILER"], str(source), "-Wl,--no-as-needed",
                        str(library), f"-Wl,--dynamic-linker={loader}",
                        f"-Wl,-rpath,{sysroot}/lib:{sysroot}/usr/lib:{foreign}",
                        "-o", str(binary)], check=True)
        trace = subprocess.check_output([str(binary)], text=True,
                                        env=dict(env, LD_TRACE_LOADED_OBJECTS="1"))
        assert str(library) in trace, trace
        assert (f"libforeign.so => {library}" in trace) == named, trace
        reject(trace, [allowed])
        check_libraries(trace, libraries, [allowed, foreign])
        reject(trace + "unrecognized loader output\n", [allowed, foreign])
        reject(trace + "libmissing.so => not found\n", [allowed, foreign])
        reject(trace.replace(str(sysroot / "lib/libc.so.6"), str(library)),
               [allowed, foreign])
print("runtime mapping regression passed")
