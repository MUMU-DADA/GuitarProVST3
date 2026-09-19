# MinHook

MinHook source and license from version `v1.3.4`, commit
`c3fcafdc10146beb5919319d0683e44e3c30d537`:
https://github.com/TsudaKageyu/minhook

The repository contains the upstream `include/`, `src/`, and `LICENSE.txt`
files, with the local changes described below.

Local changes add `MH_EnableHooksStrict`, `MH_DisableHooksStrict`, and the
read-only `MH_IsHookEnabled` state query in
`src/hook_strict.inc`, their declarations and status codes. Existing upstream
entry points remain unchanged. Each extension changes a batch whose hooks are
all in the opposite state. Disable retains trampolines so callers already in
flight can return safely; callers must not remove those hooks until they can
prove no call still uses a trampoline. Both operations retain thread handles,
checks suspend/context/resume results, rejects an instruction pointer inside any
patch range without moving it, and preflights every target page before writing.
The frozen region uses `NtGetNextThread` and preallocated memory rather than
Toolhelp, heap allocation or loader lookup. A second complete thread pass must
add no live threads; this assumes no external debugger/injector creates threads
during a patch transaction. Lack of `NtGetNextThread` disables the feature.

Before writing, enable checks the original bytes and disable checks the installed
detour bytes. Patch/cache failures restore the prior bytes and hook state before
resume when pages remain writable: original bytes for enable, complete detours
for disable. Protection restoration and resume get three attempts. A failure
after writing may leave the whole batch in the requested state; the error must
not be interpreted as proof that nothing changed. Persistent failure is
explicitly reported; callers retain all trampolines and keep their feature gate
disabled, and a restart may be required. This does not promise recovery from
arbitrary OS failure or hostile concurrent memory modification.
The query returns the recorded hook state under the internal lock, including
after a failed transaction; it does not inspect target code or change threads.

`native/test/test-p13-minhook-strict.ps1` exercises enable and disable with
enumeration, suspension, context, busy instruction pointer, page protection,
cache flush, protection restoration, and thread resume fault injection. Both
normal and hotpatch-above targets check whole-batch bytes and internal state.
It also enables and disables hooks while four real threads execute the target.
