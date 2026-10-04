# Vectis agent instructions

Follow user and developer instructions and the repository's documented lifecycle. Work on a named non-default branch, keep scratch in ignored `build/`, run operational commands serially, and preserve unrelated changes. Commit verified task-owned changes with Conventional Commits; do not push without authorization.

## Dependency upgrades and Lua integration

The Vectis executable and its Lua application surface are products. An SDK version bump, successful link, module preload, or upstream facade import does not establish a completed upgrade.

For every upgrade, the responsible agent must investigate the differences in **all** upgraded dependencies and their Vectis seams. Compare the prior pinned/released public headers, Lua modules, documentation, and examples with the candidate artifacts. Examine additions, removals, changed defaults, options, ownership, callbacks, errors, concurrency, streaming, persistence, and deployment requirements. Investigate transitive changes that affect those contracts too.

Trace each relevant change through native binding, Lua dependency facade, Vectis convenience facade, app configuration and lifecycle, process/thread placement, diagnostics/logging, docs/examples, normal execution, Linux packed execution, Lua source-rock packaging, and static/shared consumer closure. A method's presence is evidence of availability, not proof of an end-to-end workflow.

Implement the required seams in the same upgrade task. When an upstream operation crosses Vectis-owned concerns, improves an established Vectis workflow, or requires Vectis ownership/lifecycle/error handling, add or update its Vectis convenience integration automatically. Do not wait for a second user request. Preserve direct dependency access; do not add wrappers solely to rename an upstream call.

Never classify a missing integration as an inevitable property of the architecture. In particular, outbox commit receipts must reach the supervisor dispatcher through the declared bounded wake path; reconciliation is recovery, not an acceptable silent replacement for that fast path. A native notification function alone does not prove worker-to-supervisor delivery. Foreign threads must never enter arbitrary Lua states, and thread-bearing resources must materialize only after the applicable fork boundary.

If a public dependency API cannot support a required contract, report the exact blocker and prepare an upstream report when authorized. Do not hide it behind buffering, periodic polling, a staging file, an invented stub, or documentation that presents a specification as implemented behavior. Document deliberate exclusions with their concrete rationale and reconsideration trigger.

Update `docs/lua-coverage-matrix.md`, relevant manuals and examples, and the current dependency-surface audit. Add observable regression tests for successful and failed execution, cleanup, option propagation, and process/thread boundaries. Keep the first feedback path fast: deterministic faults, zero-wait probes, tiny fixtures, and no delay-based fault tests. Broader lifecycle, real service, packaging, and memory checks must still prove the promised product contract.

## Mandatory agent release gate

Before releasing any Vectis version, the responsible agent must perform and record a fresh semantic Lua/integration surface review. This is a reasoning gate; do not move it into build shell scripts or treat a symbol scanner, coverage matrix, successful build, or test exit code as a substitute.

The review must:

1. Identify the release candidate commit and dependency versions/digests, and compare with the previous released/pinned baseline.
2. Enumerate changed upstream public and Lua surfaces and relevant new bundled libraries. Check every seam listed above, including convenience integration decisions and documented exclusions.
3. Verify that promised behavior is implemented rather than merely specified. Trace representative workflows end to end, including notification/wake, error, ownership, cleanup, and concurrency paths.
4. Run the applicable regression, normal/packed execution, service, memory, and package checks; distinguish API-presence probes from behavior evidence and skipped/live-only coverage.
5. Review the implementation, fix actionable defects in a separate authorized fix iteration when lifecycle release policy requires it, and repeat the review after fixes.
6. Record findings, fixes, deliberate exclusions, commands/results, and remaining limitations against the final candidate. An unimplemented required seam or unresolved actionable issue blocks release.

The final release report must identify this agent review evidence. Release scripts cannot approve this gate on the agent's behalf.
