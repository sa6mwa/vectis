# MDF 0.19.0 upgrade

## Reviewed specification

Baseline `f5cedc2` selects MDF 0.18.0. Select all seven SDKs and the released
Lua source archive from the new authoritative repository
[c89systems/libmdf](https://github.com/c89systems/libmdf/releases/tag/v0.19.0).
Use published SHA-256 digests and retain all other dependency pins.
Reviewed source archive SHA-256:
`618266f5cd433586b147927464a927d22b5f4f5789ba2c96f79973b5845804ac`.
The release tag identifies commit
`55a3de1e40cb9f62f7b120ccefd2f8c6e4eb6eb6`.

Public header changes document zero-right-margin URL layout; signatures,
struct layout, exports and ABI 6 remain unchanged. The Lua core and module are
byte-identical to 0.18.0. The native renderer now preserves contiguous visible
URLs outside table cells with OSC8 off and a zero right margin, including
styled, scheme-relative and list destinations. Continuation rows use terminal
autowrap without inserted margin bytes; following text uses the actual cursor.
Positive right margins retain hard wrapping. Width must match terminal columns
when using natural wraps. OSC8 remains optional. Native changes are confined to
ANSI rendering and private cursor/pending state; no transitive dependency,
thread, ownership, persistence or streaming interface changes. Upstream test
builds and attribution also change; retain the released MIT/OFL notices.

Existing Vectis terminal options pass through unchanged. Smith already uses
the native bounded reader/sink thread and terminal output descriptor, including
the two-column left margin. Native docs paging retains its public contract.
No new convenience API, provider facade extension or production wrapper is
needed. Consume both released Lua files unchanged.

Implement pins, installed minimums, version assertions, manuals and audit in
one pass. Add observable URL/cursor and fragmented source/sink/trace parity
regressions through direct MDF and Vectis helpers, and a native public-API case
matching Smith's margin. Exercise ANSI URL rendering on a default musl worker.
Then run focused Debug checks, standard native checks, musl/default-stack and
memory checks, released-source provenance, embedded manuals, source-rock and
relocated static/shared consumers. Check all seven SDK native/Lua contracts;
execute Linux probes where supported, with Darwin compile/link only. Reuse
unaffected full-platform evidence from prior upgrades without relabeling it.
No release, tag or push. Specification review: complete; no decision pending.

## Delivered behavior and review

All eight pins now use the new repository, with installed CMake/pkg-config
minimums of 0.19.0. Other dependency selections are unchanged. No production C
or Lua application implementation changes are necessary. The upstream Lua core
and module remain unchanged; configured render options reach the released
renderer through the existing native, dependency and convenience interfaces.

Regression coverage checks raw contiguous URL bytes, styled fragments, lists,
quoted lists, scheme-relative addresses, positive right margins, exact-edge
cursor placement, fragmented input, and trace/sink parity. Smith's actual
render thread is checked through pipe and pseudo-terminal destinations. Existing
reader/sink/trace error identity, Vectis error translation, constructor failures,
retained font/token data, materialized result adoption and close/GC checks pass.
The combined worker invokes ANSI and HTML MDF callbacks under an active native
digest reader and exercises nested owned Lua buffers.

Review traced private cursor state through pending-emission save/restore and
soft-wrap resets. No additional ownership, cleanup, concurrency, configuration,
logging, app lifecycle or persistence seam is required. The GNU exported symbol
names are identical (34), and SONAME remains `libmdf.so.6`. Released SDK packaging
preserves the updated C89 systems MIT attribution and existing OFL notice.
The final review found no actionable product defect. Formatting changed only
test string-literal layout after the full suite; its focused Debug check passed.

## Verification

Logs, source comparison and isolated consumer fixture are under ignored
`build/mdf-0.19.0-review/`. Dependency preparation used the owned
`build/useful-lua-facades/dependency-cache` archive cache.

| Check | Result |
| --- | --- |
| Native Debug build and `ctest --test-dir build/debug --output-on-failure -j1` | **284/284 passed**, 498.04s, including real service, fork, wake-delivery, streaming, cleanup and packed workflows. |
| First focused MDF/Smith/source/worker checks | **9/9 passed**, 9.88s; normal and Linux packed MDF regression included. Final formatting-only Smith check **1/1 passed**, 0.86s. |
| Strict Valgrind MDF allocator fixture | Nine deterministic failures against the exact product's released binding/module; **7940 allocations/frees**, zero live bytes and **zero errors**, no suppressions. |
| Strict Valgrind combined worker | **115852 allocations/frees**, zero live bytes and **zero errors**, no suppressions. |
| Native x86_64 musl | Rebuilt CLI/static library, released binding and affected fixtures; **8/8 passed**, 11.54s, including normal/packed MDF, Smith, source provenance, allocation failures and worker. |
| Default worker stack | NULL pthread attributes, **133656 bytes** observed. ANSI URL and HTML streaming callbacks pass under the nested digest/buffer call chain without increasing the stack. Binding render/stream frames remain **752/608 bytes**; path helper frame **16480 bytes**. Individual frames are not a universal call-chain bound. |
| Seven SDK contracts | Fresh C89 static and C++98 shared compile/link probes and C99 released-Lua-core compilation against imported headers pass for all seven. Native static behavior probes execute on all six Linux targets, ARM under QEMU; Darwin compile/link only. Probes cover URL/margin behavior, native palette lookup and bounded-buffer failure cleanup. |
| GNU Release product | CLI/static/shared libraries rebuilt; final MDF regression passes normal and Linux packed execution. |
| Relocated GNU SDK | Generated actual archive, extracted into a new owned prefix; `scripts/verify_installed_sdk.sh` passes static and shared C89/C++/pkg-config consumers, selected 0.19 headers, native palette calls and **36 installed C examples per mode**. |
| Provenance and documentation | Actual binding compilation and exported upstream module bytes checked; both Lua files hash identically to 0.18.0. Embedded current manuals match source, including MDF and terminal manuals. Existing rendering examples pass. |
| Lua source rock | `scripts/test_lua_rock.sh` passes with owned build/tree paths. This is helper packaging evidence using native stubs; real native behavior is proved separately above. |
| Formatting | `scripts/check_format.sh` passes. |

These are seven MDF SDK contract checks, not seven new full Vectis product
builds. Full product rebuilds cover GNU x86_64 and the musl x86_64 CLI/static
surface. Previous full-platform product evidence remains historical; the ARM
app-emulation robust-mutex limitation and lack of Darwin execution remain as
documented in the lockdc review. Interactive docs paging and terminal mouse
click detection are not claimed by the noninteractive manual/byte checks.
No new facade gap, local provider patch, ABI change or release approval is
introduced. No release, tag or push is performed.
