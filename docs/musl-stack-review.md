# Musl worker-stack review

Scope: libvectis and its owned Lua bindings at `1068b39`, reviewed after
reloading the pkt-systems-cmake-lifecycle skill. The library must work on musl
worker threads with their default 128 KiB stack. A main-thread test or an enlarged
pthread stack does not prove this. Keep public APIs/ABIs and released upstream
Lua facades unchanged. This is stack-budget work, not a dependency upgrade or
an SSH feature task.

Compiler inspection uses the existing x86_64-musl toolchain with `-fstack-usage`
and production optimization in the owned `build/musl-stack-review/native` tree.
The initial largest owned native frames are WebDAV copy/move 84944 bytes,
embedded-file verification 65824, digest input 65648, four SSH file-transfer
functions about 33 KiB each, auth pruning about 17 KiB, QR encoding 16000 bytes.
Directory walkers added 4–8 KiB per level; FD-based walkers also accumulated
stack with nesting. Lua buffer, raw/streaming zlib and gzip had 64 KiB locals.
Large output/row/event arrays already inside heap-owned compression, PNG, SQL
cursor and SAX receivers are not local-stack findings.

Implementation batch: move the large owned workspaces to heap/receiver/Lua-owned
storage with existing cleanup and allocation-failure contracts; move SQL parameter
scratch off the C stack; replace internal recursive directory traversal with
iterative walks retaining only the active path on the heap. Preserve bounded
streaming, chunk sizes, file semantics and callback ownership. Use no new public
API, artificial depth limit, larger default stack or upstream facade patch.

Verification follows the complete repair batch: compiler frame readback; native
and Lua behavior on pthreads created with NULL attributes under musl; nested
digest/zlib/buffer callbacks, deep cached/direct WebDAV copy/move/delete and cleanup,
embedded-file verification and QR; allocation-failure/cleanup and existing affected
regressions. Reuse existing unrelated results and do not repeat the full target
matrix. Compiler frame sizes alone do not prove complete call-chain bounds or
arbitrary user callback recursion. Record the exercised chains and limitations.

## Repairs and ownership

Digest and embedded-file verification keep their 64 KiB chunks on the heap.
The four existing SSH file-transfer buffers and QR workspace are heap-owned and
freed after every return from their private worker. No SSH feature was added.
WebDAV's cached/direct transaction workspaces are heap-owned too. WebDAV usage,
copy and removal, including the FD-relative variants, and ACME temporary-tree
cleanup use iterative traversal with heap-owned active-path frames. Stack use
does not grow with directory depth; failed traversal closes active directories
and frees the frames. Existing file policies and transaction handling remain.

Lua stream/raw zlib and gzip keep bounded output scratch in their userdata
owners. Buffer reads use temporary Lua-owned scratch. SQLite/PostgreSQL Lua
parameter arrays use Lua-owned scratch for the bounded parameter domain; native
PostgreSQL's vectors belong to its serialized connection. Lua allocation failure
therefore cannot bypass a native free for these scratch areas. Native engine
buffers already inside heap-owned receivers remain there.

Final review found that sizing Lua scratch from a table's current length would
be incorrect: allocation may run a finalizer that grows the table before the
parser reads it again. Both SQL bindings reserve the existing maximum of 256
parameters, then retain the ordinary parser's validation. Deterministic SQLite
and real PostgreSQL tests grow the table from one to 256 entries at allocation,
force GC and verify the final row. This is an ownership fix, not a new API.

The existing gzip OOM regression caught an initial per-read allocation that
changed when failure occurred. Moving scratch into the file owner preserves the
old after-I/O failure contract: a failed result copy closes the adopted FD and
invalidates the receiver. `vectis_unit_gz_lua_oom` exposes that precise path as a
fast standalone test; it does not suppress or replace the full gzip test.

## Compiler readback

Representative optimized x86_64-musl frames, in bytes:

| Path | Before | After |
| --- | ---: | ---: |
| Cached WebDAV copy/move worker | 84944 | 464, plus small owning wrapper |
| Digest input driver | 65648 | 96 |
| Embedded-file verification | 65824 | 64 KiB local removed; largest embedded-FS frame is 608 after inlining |
| Existing SSH transfer workers | 33008–33152 | 224–384, plus 64-byte owning wrappers |
| QR encoder worker | 16000 | 320, plus owning wrapper |
| PostgreSQL send | 8400 | 144 |
| Lua buffer read | 64 KiB local buffer | 608 |
| Lua zlib stream/step | 64 KiB local buffers | 752 / 688 |
| Lua raw zlib process | 64 KiB local buffer | 672 |
| Lua gzip driver | 64 KiB local buffer | 1168 |
| WebDAV recursive walkers | 4304–8432 per level; FD variants 368–464 per level | Iterative; usage/removal/copy frames 208–384, independent of depth |

The largest remaining measured native library frame is 17968 bytes, in WebDAV
put; auth pruning is about 17 KiB. The owned Lua storage-default helper is 17024.
These are individual frames, not complete call-chain bounds. The stream test
keeps the native digest reader active while Lua drives gzip, raw zlib, buffer
copies, curl file callbacks and Zstd/LZ4/XZ. Buffers stay owned across those
callbacks instead of accumulating 64 KiB native frames. Deep WebDAV tests cover
quota scanning, copy/move, readback, early conflict and removal in both storage
modes; iterative traversal retains only the active path on the heap.

The profiler also measures CLI-only main-thread commands: Smith command 152576
bytes, pack/unpack about 22–25 KiB. Those translation units/entrypoints are not
part of libvectis or the tested pthread workflows. Their main-thread evidence
must not be presented as a default worker-stack guarantee. No upstream binding
source was patched. Native SDK internals are exercised through the workflows;
their archive objects do not acquire compiler frame reports from this build.

## Verification and limits

- Both `vectis_unit_stack_worker` and `vectis_unit_webdav_stack_alloc` pass on
  x86_64-musl natively, and aarch64-musl/ARMhf-musl under QEMU. Workers are created
  with **NULL pthread attributes**; no stack size is requested or enlarged.
  Reported actual stack sizes are 133656, 133640 and 132048 bytes respectively.
  Static musl test linking makes the fixtures executable on this glibc host;
  it does not change their pthread stack attributes.
- The worker exercises a 64 KiB digest/Lua/zlib/buffer/curl/codec callback chain,
  maximum-count SQLite parameters, PNG-to-PDF, DOM/XPath/SAX, AES-GCM, embedded
  extraction/verification, QR and 160-level WebDAV trees. Each Lua VM and native
  resource is created on its owning worker, not entered from a foreign thread.
- WebDAV fault tests cover 13 cached and 10 direct allocation positions. They
  inject one recoverable allocation failure at a time, require balanced owned
  heap and FD counts, preserve source data and check failed destinations.
- The affected debug selection passes **30/30 in 14.14 s**, including normal/
  packed Lua, native WebDAV/atomic/embedded-FS/QR, SQLite/zlib allocation failures
  and the PostgreSQL service workflow. The added dedicated gzip OOM check and
  the full normal gzip unit also pass. Unchanged broad/matrix evidence is reused;
  no seven-target release rebuild is claimed for this repair.
- After the final bounded-parameter/GC review fix, the four affected debug checks
  pass in 9.09 s, including the real PostgreSQL mutation regression. The final
  x86_64-musl default-stack pair passes again in 1.09 s. The ARM results cover
  the completed stack-repair batch before this heap-capacity-only correction;
  their exercised maximum-count parameter path already used the same capacity.
  No ARM/platform rebuild was repeated for this focused review fix.
- Strict native memory checks report zero bytes at exit and zero errors for
  the worker (71811 allocs/frees), WebDAV faults (5237), SQLite OOM, raw-zlib OOM
  and isolated gzip OOM (453). Final SQLite memory checking includes the growing
  parameter-table regression: 596494 allocations/frees, 54 injected failures and
  714 completed runs. No suppressions are used.
- The broader native gzip memory probe still reports one already-documented
  upstream zlib 1.3.2 `gztell64` uninitialized-value branch and frees all 501
  allocations. It is not recorded as clean. See `zlib-public-facade-spec.md` and
  its existing pure-provider `--native-tell` reproducer. The gzip native engine
  and SDK were not patched as part of this stack repair.

Compiler reports are in `build/musl-stack-review/final-frames.tsv`; build, CTest
and memory logs are adjacent. Reproduce with the x86_64-musl preset plus
`-DCMAKE_C_FLAGS=-fstack-usage`, tests/binary enabled, and static fixture linking;
build the two stack targets and select `ctest -R '^vectis_unit_(stack_worker|webdav_stack_alloc)$'`.
ARM tests use the existing cross presets and CMake's QEMU emulator setting.

Final review: the identified oversized owned workspaces and depth-dependent
filesystem C-stack growth are repaired. Bounded chunks and public APIs/ABIs are
unchanged. Arbitrary application callback recursion and caller stack consumption
still require the caller's own budget; these tests are observable representative
workflow evidence, not a mathematical bound for every user callback, SDK plugin
or calling context. This is not release approval.
