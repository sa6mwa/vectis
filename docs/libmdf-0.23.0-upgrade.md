# MDF 0.23.0 upgrade

## Reviewed specification

Upgrade baseline `a7b0ac8` from MDF 0.19.0 to the seven released 0.23.0 SDKs
and Lua source archive from
[c89systems/libmdf](https://github.com/c89systems/libmdf/releases/tag/v0.23.0).
Use the published SHA-256 digests; retain every other dependency selection.
Reviewed source archive SHA-256:
`25221c25244ac008f3c338091d03db2aebdcde519669dbb2323cfaa6df7476e9`.

The dependency ABI advances from 6 to 7. Pager entry points now take optional
cooperative cancellation controls; new C APIs expose seekable borrowed sources
and rendered-cache configuration. The released Lua facade adds cancellation and
cache options to file paging, literal writes, Markdown escaping and configurable
pending-word storage. Consume it unchanged. Existing Vectis Markdown helpers
continue forwarding render options; persistent receiver operations stay directly
accessible without redundant convenience wrappers. Vectis's own public C ABI
does not expose an MDF receiver/options layout and does not change.

The intervening releases also add xterm OSC8 detection when specific terminal
hints are absent, preserving explicit disable and older-terminal hints. There
are no new linked dependency, persistence or background-thread requirements.

Replace Vectis docs paging's forward-only source with a seekable view of the
existing immutable embedded resources and generated headings. Do not concatenate
or spool the manual. Reads and seeks must handle every section boundary, EOF,
invalid offsets and sources larger than 1 MiB, using bounded workspace. Preserve
plain docs output. Use MDF's default rendered cache and borrowed source lifetime;
the call remains synchronous. Cancellation must restore the terminal and return
an interrupt exit status. Add native source tests and actual pseudo-terminal
coverage for docs paging and upstream Lua cancellation/cache/error behavior.

Rendering now bounds full ANSI/HTML table retention before continuing in row
mode. Pending words default to 4 KiB memory and optional temporary backing up to
1 MiB per word; excess bytes are discarded until the word boundary. This is bounded
layout-decision storage, not a whole-document spool. Document those limits and
the explicit disable-spooling/truncation choice. Test fragmented literal/escaped
input, intraword underscores/NBSP, word-boundary decisions, spool/truncation and
error cleanup, plus HTML table output before source EOF. Keep prior ownership,
callback errors, option propagation and normal/packed regressions.

Implement this entire surface before broader verification. Then run focused
Debug tests, the standard native suite, default musl worker checks, strict memory
checks, actual upstream-source/header provenance, embedded manual/source-rock
checks, and relocated static/shared consumers. Verify all seven released SDK
public/native/Lua contracts; distinguish cross compile/link from actual runtime
evidence. No release, tag, push or sibling-repository modification. Preserve the
unrelated staged `LICENSE` change. Specification review: complete.

## Implementation and evidence

All eight artifact pins now select the verified 0.23.0 archives. Exact build
requirements and installed CMake/pkg-config minimums were updated; other pins
remain unchanged. The relocated SDK contains `libmdf.so.7`, with no additional
native dependency beyond libc. The public header, released Lua module/core,
guides and examples were compared against 0.19.0. No dependency source was
patched or supplemented with a local provider method.

Docs paging uses a private borrowed-resource adapter with bounded heading
scratch and absolute seeks across headings, bodies and separators. Invalid or
unrepresentable offsets fail without changing the read position; exact EOF is
valid. It invokes `mdf_pager_seekable_source` with the default cache and returns
130 for `MDF_CANCELLED`. Plain docs output is unchanged. Existing terminal
helpers and Smith keep their render options, synchronous callbacks and owner
lifetimes. The new upstream receiver operations need no additional Vectis Lua
wrapper. Native palette lookup remains C-only, with no current Lua workflow
requiring a provider extension.

Verification completed on 2026-11-07, with operational commands serialized:

| Check | Evidence |
| --- | --- |
| GNU Debug standard suite | **292/292 passed**, 483.58 seconds, including PostgreSQL/XA services, normal/packed execution, process/ownership boundaries and runtime closure. |
| Rendering regressions | Literal writes and tokens, materialized escaping, memory-only truncation and file-backed pending words, receiver/document setter rejection while pending, NBSP/intraword underscores, early HTML table rows, callback error identity and existing URL/ownership fixtures pass. Final UTF-8 fragment assertions passed again in Debug, musl Release and GNU Release, including normal/packed runs. |
| Docs source | A source larger than 1 MiB passes bounded reads, exact byte comparisons, empty-document boundaries, backward seeks, EOF and invalid/overflow offsets. |
| Actual interactive pager | Pseudo-terminals exercise embedded docs, ignored-SIGINT cancellation with exit 130, Lua callback exceptions and cancellation, invalid cache budgets, subsequent ordinary quit and canonical/echo/alternate-screen restoration. File fixtures exceed 2 MiB with a 64 KiB cache. |
| Seven SDK contracts | C89 static and C++98 shared compile/link, private docs-adapter compilation, and C99 released Lua-core compilation pass for all seven SDKs. All six Linux targets pass **4/4 behavior checks each**: static/shared API behavior, docs source and default-thread paging. ARM uses QEMU; Darwin is compile/link only. These are SDK checks, not seven new full Vectis builds. |
| GNU/musl Release products | Executable, static/shared libraries and public-header checks built on x86_64 GNU and musl. GNU Release MDF regressions pass normal/packed execution. |
| Musl runtime and stack | Focused product selection **13/13 passed**, 11.35 seconds. The combined Lua/native worker reports **133,656 bytes**, with NULL pthread attributes, exercising file-backed literal decisions and nested native digest/Lua/buffer callbacks. Dedicated native pager workers also pass with default stacks on x86_64 musl and ARM musl under QEMU. No stack size was increased. |
| Strict Valgrind | Final rendering fixture: **121,270 allocations/frees**; eleven deterministic Lua allocation failures: **9,904**; combined worker: **143,168**. Each has zero live blocks and zero errors, without suppressions. All leak kinds are fatal. |
| Pager memory checks | **3/3 passed**, 10.58 seconds, using the dynamic Debug harness under CTest with child tracing. Lua paging/cancellation/reuse, embedded docs and a native pager worker leave zero live blocks/errors in all six parent/child logs. |
| Relocated GNU SDK | Fresh development archive extracted into a new prefix. Static/shared C89/C++98/pkg-config consumers exercise the new word-buffer and escaping calls, and **36 installed C examples per mode** pass their runtime contracts. |
| Provenance/docs/rock | Actual upstream binding compilation and embedded Lua source bytes match the selected release. Updated README and MDF/terminal/self-description manuals match embedded bytes and SHA-256 digests. Lua source-rock staging/smoke passes; its native stubs are packaging evidence, separate from real native execution above. |
| Formatting | `scripts/check_format.sh` and `git diff --check` pass. |

The initial focused run exposed an unsupported automatic-close assumption in
the new test; it now uses MDF's explicit `close` contract. Musl test executables
were relinked with the documented static test setting so they run on this glibc
host. A manually invoked worker needed the same absolute script path used by
CTest, since its curl probe builds a file URI. These were fixture/invocation
corrections, not MDF defects. Extra memory runs use the dynamic Bootlin harness
for allocator/libc interposition; an instrumented docs load exceeded the original
five-second readiness window, so the new harness allows 30 seconds while CTest
owns cancellation. Final Debug/musl pager selections each pass **4/4** after that
test-only refinement. No shipping implementation changed after the full native
suite, and no full platform rebuild was repeated for test-only refinements.

Review found no actionable integration defect or required upstream handoff.
Streaming decisions, backing-file limits, table retention, pager replay and
cancellation limits are explicit in the manuals. Vectis adds no whole-document
buffer, background pager thread, retry engine or compatibility shim. Upstream
private crash/fault/fuzz suites and interactive mouse behavior were not rerun;
these results are representative workflow evidence, not a whole-process RSS or
arbitrary-callback stack bound. No release, tag or push was performed.

Scratch sources, comparisons, commands, artifacts and logs are under ignored
`build/mdf-0.23.0-review/`. Reproduction uses `scripts/deps.sh`, the `debug` and
x86_64 GNU/musl Release presets, `ctest --test-dir <build> -j1 --output-on-failure`,
`scripts/verify_installed_sdk.sh <relocated-prefix> static|shared`,
`scripts/stage_lua_rock_sources.sh`, and strict Valgrind options
`--error-exitcode=99 --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all`.
The scratch SDK probe is a native CMake/CTest project using the repository's
seven toolchains. The unrelated staged `LICENSE` change remains excluded.
