# Forked runtime exit ownership

Reviewed implementation contract, 2026-10-27. A focused service Memcheck run
shows the owning CLI releases its Lua runtime, while the forked Kore parent and
workers exit with their copied VM still alive. This is separate from Kore's
uninitialized IPC header padding. Do not suppress either diagnostic.

Add an app-configured, optional C89 `vectis_app_set_fork_exit_handler` callback.
Configure it before start. It runs once on the exiting process's owner thread,
after Kore connections/modules have stopped, immediately before that forked
Kore parent/worker exits. Role strings are `kore_parent` and `kore_worker`.
It does not run in the original application process or on arbitrary threads.
A callback may reclaim the app; the invoker must detach the callback before
calling it and never access the app afterwards. Failure makes that child exit
unsuccessfully. Native CMake/CTest owns cancellation, without a PID supervisor.

The embedded CLI installs its own handler when constructing each app. It
protects the optional Lua `app:on_fork_exit(function(role) ... end)` callback,
then always releases the process's copied Lua runtime using the public cpkt
runtime API, followed by its embedded filesystem owner. Clear the context's
runtime pointer before closing it. No Lua operation may follow VM destruction.
The callback and userdata stay rooted until close; replacing/clearing the Lua
callback releases its reference. Lua lifecycle operations are rejected during
the user callback. Changing this user callback cannot disable CLI ownership
cleanup. Normal original-process runtime close follows the same owning helper.

The C library remains Lua-independent. Its app handler and internal invocation
are explicit; no weak symbol lookup, process-global atexit handler, or switch
from generic fork `_exit` to `exit` is introduced. Kore's tracked patch invokes
the worker cleanup only after native resource teardown. The app's fork branch
invokes parent cleanup after `vectis_internal_kore_run` returns. Foreground
execution returns normally to Lua; it must not destroy the caller's VM.

Prove the C callback's configuration/lifetime contract, observable Lua role
callbacks, original-owner exclusion, normal and packed shutdown, callback
failure, and strict service Memcheck. Existing HTTP callback/worker wake tests
remain required. Finalizer failures, native UAF, outstanding handles or missing
child memory summaries are actionable failures, not successful cleanup.
