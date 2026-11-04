# Skyline Compatibility — Project Handoff

Last updated: 2026-11-02
Repository: ioio1o/p5r-android_test
Branch: feature/skyline-plugin-dry-run
Target: Persona 5 Royal 1.0.2, Android NCE
Title ID: 01005CA01580E000
Build ID: D4B150B29A931CD3

This document preserves the implementation findings, architectural decisions,
diagnostic history, current investigation state, and next steps. Update it when
important findings are confirmed. Do not replace confirmed observations with
untested assumptions.

---

## 1. Project objective

Build a native Android port of Persona 5 Royal with NCE execution and general
Skyline plugin compatibility.

The goal is not merely to make one plugin work. The intended experience is
Eden-like: users install compatible plugins and mods in the expected directory,
launch the game, and the runtime loads them without plugin-specific patches.

The implementation should remain compatible with the existing NCE architecture.
Do not attempt to reproduce an entire Switch emulator architecture unnecessarily.

Important supported goals:
- General NRO plugin discovery and loading.
- Skyline HLE and AArch64 hook compatibility.
- ExeFS and RomFS mod support.
- Multiple plugins working together.
- Straightforward copy-and-launch installation.
- General runtime fixes instead of plugin-name-specific workarounds.

P5R-specific features such as 60 FPS and PC settings are not the current target
for the p5rcbt / P5R Bonus (Downgraded) plugin. Keep those features disabled.

## 2. Development environment and workflow

### User environment
- Android device: Poco F5, 8 GB RAM.
- SoC: Snapdragon 7+ Gen 2.
- GPU: Adreno 725.
- OS: LineageOS 23.2 Neko.
- Development shell: Termux.
- Repository path: `~/p5r-android_test`.
- Game: Persona 5 Royal 1.0.2.
- User performs Termux commands and supplies output for inspection.

### Build environment
- Android NCE build runs through GitHub Actions.
- Do not ask the user to build locally.
- Do not claim a build succeeded without checking its run status.
- Pushing a commit does not automatically dispatch the required build.

### GitHub workflow
- Repository: `ioio1o/p5r-android_test`
- Branch: `feature/skyline-plugin-dry-run`
- Workflow file: `build-apk.yml`
- Workflow ID: `399319460`
- Dispatch:
  `gh workflow run 399319460 --ref feature/skyline-plugin-dry-run`
- Check runs with `gh run list` and `gh run view`.

The GitHub connector can fetch branch-specific source files. Use the explicit
branch ref when inspecting:
`feature/skyline-plugin-dry-run`.

GitHub connector writes previously returned 403. Do not assume direct connector
writes are available.

### Required change workflow
1. Inspect the current branch source and preserve established discovery.
2. Prepare a narrowly scoped patch.
3. User applies it in Termux.
4. Inspect the actual diff and command output.
5. Stage only intended files. Never use `git add -A`.
6. Commit and push to the feature branch.
7. Manually dispatch the GitHub Actions build.
8. Verify the run and tested revision before claiming success.

Do not stage unrelated untracked files.

### Termux-specific rules
- Never use `/tmp` or a `tmp` directory.
- Avoid huge or truncated diffs.
- Existing diff configuration:
  ```
  git config diff.algorithm histogram
  git config diff.colorMoved no
  git config color.diff false
  git config core.pager cat
  git config alias.dword 'diff --no-ext-diff --no-color --word-diff=porcelain --unified=0'
  git config alias.cword 'diff --cached --no-ext-diff --no-color --word-diff=porcelain --unified=0'
  ```
- `git dword -- <file>` displays unstaged changes.
- `git cword -- <file>` displays staged changes.
- In `git status --short`, `M ` means staged only.
- Prefer bounded source inspection using `nl -ba file | sed -n 'start,endp'`.
- Preserve the existing discovery process; do not repeatedly ask for diagnostics
  that are already available in project history or repository files.

### User interaction expectations
- Keep instructions concise and batch related diagnostics.
- The user has limited chat quota.
- Do not claim to have run Termux commands; the user operates the device.
- Do not invent paths, output, commit IDs, build results, or runtime observations.

## 3. Core runtime architecture

### Plugin discovery and loading
- Plugins are discovered under:
  `mods/*/romfs/skyline/plugins/*.nro`
- Plugins are loaded as additional NCE modules.
- Reserved plugin module slots occupy 256 MB from:
  `0x4030000000`
- Plugin relocations are processed after the main game and SDK modules are
  linked.
- Plugin symbol resolution checks:
  - Plugin's own symbols.
  - HLE symbols.
  - Main game symbols.
  - SDK symbols.
- Plugin exports are not inserted into the global `g_exports`.
- Plugin loading and NCE preparation precede:
  - `DT_INIT`
  - `init_array`
  - exported `main`

### Runtime module data
`SkylinePluginRuntime` contains:
- Plugin name.
- NRO image.
- Names, symbols and relocations.
- `ModuleInfo`.
- Base address and mapped size.

`ModuleInfo` fields:
- `text`, `text_size`
- `ro`, `ro_size`
- `data`, `data_size`
- `bss`, `bss_size`

`LoadedModule` contains:
- `.base`
- `.size`
- `.info`

Important classification detail:
`skyline_plugin_from_address()` currently recognizes plugin text only.
It must not be used alone to classify plugin data or BSS addresses.
Correct memory classification must check text, ro, data and bss segment ranges.

### Filesystem
- `sd:` maps to:
  `Persona 5 Royal/sdmc`
- Settings are under:
  `Persona 5 Royal/p5r/`
- Current settings files:
  - `skyline_hook_mode.txt`
  - `skyline_plugins.txt`
  - `skyline_plugin_phase.txt`

### Game version
- `GetDisplayVersion` returns `1.0.2`.

## 4. Skyline API and hook implementation

Implemented Skyline HLE functionality includes:
- `getRegionAddress`
- `skyline_tcp_send_raw`
- `A64HookFunction`
- `A64InlineHook`
- `sky_memcpy`

Hook modes:
- `off`
- `passthrough`
- `full`

There is plugin allowlisting and hook-hit instrumentation.

### Hook behavior
- `nce::hook()` uses the port's generic `nx::hook_function` path.
- Its counter is named `inline`, which is misleading.
- `hook_guest()` is the Skyline `A64HookFunction` path and increments `func`.
- Plugin exports are not globally inserted into `g_exports`.

### Inline callback paths
There are two inline callback paths:

1. Special BL trampoline callback:
   `skyline_inline_hook_callback`
   - Installed only when the `A64InlineHook` target is
     `g_main->base + 0x8a9134`.
   - This is the p5rcbt bind hook.

2. Generic instruction trampoline callback:
   `skyline_generic_inline_hook_callback`
   - Used for other Skyline `A64InlineHook` registrations.
   - Uses `InlineCtx` containing x[31], SP, v[32], with size `0x300`.
   - Generic callback preserves/restores x30 and SP after replacement.

Both paths have callback tracing and invocation counters.

### InlineCtx and NCE details
- `nce_common` returns to entry `x30`; callback edits to `x30` are ignored.
- Vector registers, flags and FPCR are saved/restored.
- Thread stacks are not represented in the VM map.
- Do not validate SP-relative memory with `vm_query`.
- Generic callback context address is:
  `(original_sp - sizeof(InlineCtx)) & ~0xF`
- The context is allocated on the guest-call stack.
- Do not assume x21 or another plugin register points to InlineCtx without
  checking the actual address range.

### Hook address attribution
`skyline_plugin_address_label(address, return_address)` attributes plugin
caller/replacement addresses to NRO names and offsets.

Function and inline hook registration logs include:
- API.
- Owner.
- Caller.
- Target.
- Replacement.

## 5. NCE and diagnostic lessons

- `skyline_plugin_from_address` checks plugin text only.
- `skyline_module_contains` is used for main/SDK module checks.
- `module_at(addr)` does not automatically include plugin ranges in `g_modules`.
- Classify plugin data/BSS by explicit segment ranges.
- `V128` uses `u64[2]`.
- `nce_common` return behavior makes x30 preservation important.
- Thread stack addresses cannot be assumed to exist in the VM map.
- `sd:` file logging is capped and drops lines.
- Prefer hook counters, `/proc/<pid>/fd`, and `strace` `pread64` for evidence
  over relying on individual log lines.
- `sky_memcpy` pointer writes and patch watcher instrumentation exist.
- Pointer counters were zero in observed sessions.

## 6. ExeFS, RomFS and CPK status

### Importer
- Importer supports update patch NCA and title IDs.
- ExeFS can be read from the patch.
- BKTR patched RomFS is applied over base RomFS.
- Base sparse RomFS without its update is rejected.
- Android P5R CMake guard requires `NX_NCE=ON`.

### CPK and mods
- CPK binding works end-to-end.
- All ten Bonus CPKs were bound/read.
- Translation CPK works.
- Custom outline colors work.
- Joker calendar-day announcements work.
- Skip-prologue quickstart works.
- `225_p5r_capacity.cpk` game read was observed.

### Game image policy
- Strict game image path is used.
- No extracted ExeFS fallback.

## 7. p5rcbt / P5R Bonus plugin investigation

### Relevant addresses
- `main+0x8a9134`: p5rcbt bind inline hook.
- `main+0xcc2d1c`: Bonus generic inline hook target.
- `main+0x1048ee0`: port's `interval_setter_hook`, not a Bonus inline hook.
- p5rcbt PUT/PUTS/PUTF fixes:
  - `main+0xe33520`
  - `main+0xe33540`
  - `main+0xe33560`

Bonus plugin:
- `libp5r_bdsm.nro`
- 12 function hooks.
- Four known generic inline hooks:
  - `main+0xcc2d1c`
  - `main+0x5f6b34`
  - `main+0x4c0308`
  - `main+0x4d7b40`
- `main+0x1048ee0` is not one of these Bonus inline hooks.

### Hook registration and dispatch
Dispatch-slot diagnostics recorded:
- `free` resolves to main libc.
- `GenerateRandomBytes` resolves to SDK.
- `pthread_getspecific` resolves to SDK.

### Callback path observations
The relevant Bonus callback:
- Game hook target: `main+0xcc2d1c`.
- Plugin callback/replacement: `libp5r_bdsm.nro+0xdb58`.
- Generic inline callback instrumentation identifies this replacement.
- Callback can be invoked once during title-screen option interaction.
- Caller observed:
  `libp5r_bdsm.nro+0xfc30`
- Replacement:
  `libp5r_bdsm.nro+0xdb58`
- `changed_registers=0` on the original context for one observation.
  This does not exclude memory/global side effects.

`cutscene_outfits` means consistent worn outfit during cutscenes, not additional
outfit sets. Consistent outfit behavior was observed in a cutscene.

### Previously installed plugin trace probes
The current branch has probes at these NRO load-base-relative offsets:

```
+0x0db58 callback-entry
+0x0dc10 callback-after-helper-B
+0x0dc2c callback-after-helper-A
+0x0dc40 callback-loop-block
+0x0dc74 callback-state-write
+0x0dc7c callback-state-after
+0x0dca8 callback-after-helper-3caa0
+0x0dcac callback-config-write
+0x0dcb0 callback-config-after
+0x18264 helper-18264
+0x165b0 helper-B
+0x165e0 helper-B-after-tls-1
+0x165fc helper-B-after-init
+0x16604 helper-B-after-tls-2
+0x16624 helper-B-rng-pre
+0x16628 helper-B-rng-post
+0x16530 helper-A
+0x1598c transform-1598c
+0x3caa0 helper-3caa0
+0x05420 helper-5420
```

Probe capacity is 32. Current detailed probe logging is limited to the first
eight hits per probe.

### Observed callback trace
One recorded invocation:
- Callback entry `+0xdb58`.
- Helper B `+0x165b0`.
- Continuation `+0xdc10`.
- Loop block `+0xdc40`.
- Callback exit.

Observed registers included:
- Entry: `w8=7`, `x8=7`, `x19=0x503ee90d00`, `x0=0x385`, `x1=2`.
- Helper B: `x0=0x500239f610`, `x1=2`, `x8=0x385`,
  `x19=0x503ee90d00`, `x20=0x130`, `x21=0x4040052000`,
  `x22=0x40022c8000`.
- Continuation `+0xdc10`: `x0=0x5000019000`,
  `x1=0x500239f538`, `x8=2`.
- Loop `+0xdc40`: `x0=0x5000019000`, `x1=0x500239f538`,
  `x8=2`, `x19=0x5000019000`, `x20=0x5000019010`,
  `x21=0x4040052000`, `x22=0x960c8cf1`.
- Callback exit restored original callback context values.

In that invocation:
- No hits for helper A (`+0x16530`).
- No hits for transform (`+0x1598c`).
- No hits for `+0xdc2c`.
- No `random-bytes invocation=` line was observed.
- This does not establish whether helper B's internal RNG call occurred.

### Random index selection
User clarified the interpretation:
- 32 random bytes are produced.
- Callback extracts an index using:
  - `lsr x8, x22, #32` at `+0xdca8`.
  - `strb w8, [x21, #212]` at `+0xdcac`.
- `+0xdc8c` is Rust Rc drop glue, handling strong count at `[x19]` and weak
  count at `[x19+8]`. It is not a decision in random index selection.
- Do not revert to the earlier TLS/RNG suspicion without new evidence.

The exact multiplication instruction and its two source operands are not yet
identified in the retained disassembly excerpt.

### Current missing artifact
Need the loaded plugin instruction window:
- `libp5r_bdsm.nro`
- NRO load-base-relative offsets `+0xdbf0` through `+0xdc40`.
- Include all instruction words and offsets.
- The purpose is to identify the `mul`/`umull` instruction and both source
  registers feeding the random-index calculation.

Best method:
- Add a temporary diagnostic in `runtime/src/main.cpp` during
  `skyline_install_bonus_trace_probes()`.
- Read instruction words directly from the loaded module at
  `module.base + offset`.
- Log each plugin-relative offset and raw 32-bit instruction word.
- This avoids guessing NRO file offsets versus loaded text addresses.
- Decode the AArch64 instructions after obtaining the words.
- Remove the temporary disassembly instrumentation after the relevant
  instruction and operands are established.

Do not ask the user to reproduce earlier gameplay traces. The missing item is
the instruction window, not the entire callback trace.

### Desired store-time instrumentation
At the instruction boundary `+0xdcac`, the probe runs before the original
`strb` instruction. At that point:
- `x21` is the base pointer.
- `x22` contains the earlier input used by the shift at `+0xdca8`.
- `x8` should already contain the shifted result from `+0xdca8`.
- The low byte of `x8` is the value about to be stored.

At `+0xdcb0`, the preceding `strb` has executed. This can be used to observe
the post-store memory.

Capture for the first 20 relevant invocations:
- `x21`.
- Full `x22`.
- Both multiplication operands (after disassembly identifies them).
- `x8` and the byte that will be stored.
- Address `x21+212`.
- 64 bytes at `x21+192` before and after the store.
- Classification of x21 as:
  - InlineCtx,
  - main/game module,
  - SDK module,
  - plugin text/ro/data/bss,
  - or unclassified address.

Do not claim plugin ownership based only on `skyline_plugin_from_address`,
because that helper recognizes plugin text only.

### Delayed memory observation
Desired observations:
- Re-read the captured address five seconds after the store.
- Re-read when the title screen appears.
- Log if the selected byte changes.
- Avoid sleeping inside the NCE callback.
- Use a safe asynchronous worker or another non-blocking mechanism.
- Do not claim title-screen event detection exists until a reliable event or
  state indicator is identified. The callback is title-screen-related, but
  the exact later screen-appearance signal remains to be confirmed.

### Five-boot verification
After the diagnostic build:
1. Install the APK produced by GitHub Actions.
2. Run the same title-screen procedure for five independent boots.
3. Preserve logs for each boot.
4. Record stored-byte values and distributions.
5. Compare store-time, five-second and title-screen observations.
6. Compare the 64-byte object snapshots.
7. Determine whether the selected byte is stable, varies, or is overwritten.

Five boots provide useful behavioral evidence, not automatic proof of root
cause. The multiplication operands and snapshots must be interpreted together.

Possible outcomes:
- Same byte in all boots: repeatable selection; investigate whether inputs
  are also repeatable before concluding it is fixed.
- Different bytes: selection varies across launches.
- Byte changes after five seconds: later code modifies the target.
- Byte changes at title-screen appearance: a later event may overwrite state.
- Byte remains stable: selection persists over the observed interval.
- Unexpected multiplication inputs/results: inspect the responsible helper
  or caller.

Keep the investigation bounded:
- One focused diagnostic build.
- Five boots.
- One consolidated analysis.
- Further instrumentation only if the data identifies a specific unresolved
  behavior.

Do not hardcode Bonus randomization behavior into the port.

## 8. Prior diagnostic work and commits

Important commits on the feature branch:

- `2f22c326afd330f83652a555b3f79dc9e30d9654`
  `Add Skyline pointer hook counters and patch watcher`

- `7105e349b2f869e4cfa7bee7bd3a75710dbe6a20`
  `Fix pointer veneer module base declaration`

- `94e09cf59e7ded092b902b279befb06c93407b90`
  `Add detailed Skyline inline callback tracing`

  Added PluginTraceScope, HLE function args/results, IPC service/command
  return codes and bounded input/output bytes, filesystem path/parameter
  logs, InlineCtx before/after snapshots, memory inspection and instruction
  words around `main+0xcc2d1c`.

- `19b0f4d6e606f16dfa21d37ecb12a989750834f2`
  `Trace Bonus plugin dispatch targets`

- `c96939a`
  `Trace Bonus callback RNG and memory state`

  Added callback RNG and object/config snapshots, including 256-byte x19
  snapshots, plugin config bytes, callback registers and RNG buffer tracing.

- `1ceed1e324a5c9a472cd3d400f056dd334204d12`
  `Trace Bonus plugin callback execution`

  Added explicit module overload for plugin inline probes and ten callback/
  helper probes.

- `f74ed645ec7b98ff093e7e007081971ec24fa7d5`
  `Trace Bonus callback RNG and state writes`

  User confirmed commit and push. Build was dispatched. A run was observed
  in progress with abbreviated ID `4144…`; exact final conclusion and head
  SHA were not retained here. Do not claim the exact build result without
  checking GitHub Actions.

Other prior work:
- Hook counters and AOC diagnostics were committed and tested.
- Configurable plugin phase was committed and tested:
  `skyline_plugin_phase.txt`, default `after_ctors`, optional `before_ctors`.
- No observed change to title randomization or DLC outfit behavior from
  changing plugin phase.

## 9. Known pointer and patch observations

Four `sky_memcpy` pointer writes observed:
- `main+0x4b3e38`: target `0x404000ffa8`, veneer `0x4003200590`
- `main+0x4b3f10`: target `0x404000fffc`, veneer `0x40032005b8`
- `main+0x4b3e88`: target `0x4040010050`, veneer `0x40032005e0`
- `main+0x4b3d98`: target `0x400070c254`, veneer `0x4003200608`

Pointer counters were zero in observed sessions; patch watcher showed no
changes.

`main+0xe22d78` is a four-byte instruction patch:
`[0b 00 00 14]`, not a pointer.

## 10. Comparison with Eden

This is a qualitative architecture comparison, not a measured compatibility
benchmark.

Our implementation is a targeted Skyline-compatible runtime integrated into
the P5R Android NCE loader. Eden is a general-purpose Switch emulator with a
broader and more mature compatibility environment.

Areas already implemented in this port:
- NRO plugin discovery.
- Plugin loading and relocation.
- Main/SDK/plugin symbol resolution.
- Selected Skyline HLE APIs.
- A64HookFunction and A64InlineHook.
- Plugin initialization lifecycle.
- `sd:` filesystem mapping.
- Hook attribution and diagnostics.

Areas where broad Eden-level compatibility is not established:
- Complete Skyline HLE API coverage.
- General kernel behavior.
- Threading, synchronization and TLS edge cases.
- Broader IPC service and command support.
- General memory management equivalence.
- Hook trampoline edge cases and multi-plugin interaction.
- Broad plugin compatibility testing.

Do not claim feature parity with Eden. Successful execution of the current
Bonus plugin does not prove general Skyline compatibility.

## 11. Development roadmap

### Stage 1 — Verified execution correctness
Current stage:
- Complete Bonus callback disassembly and operand identification.
- Verify store-time register and memory state.
- Run five-boot test.
- Close the investigation if evidence is internally consistent.

### Stage 2 — General Skyline compatibility
- Audit HLE API coverage against Eden's implementation.
- Identify missing kernel services.
- Improve TLS, synchronization and thread behavior.
- Expand IPC and filesystem compatibility.
- Validate relocation and memory edge cases.

### Stage 3 — Broader plugin compatibility
- Test independent plugins with different API requirements.
- Exercise multiple plugins together.
- Fix reusable runtime deficiencies.
- Avoid plugin-specific workarounds unless required by a documented
  compatibility difference.

### Stage 4 — Eden-like user experience
- Simple plugin installation.
- Predictable directory structure.
- Clear configuration and allowlisting.
- Useful error reporting.
- Copy-and-launch workflow.

The destination is general plugin compatibility and usability, not merely
making the current Bonus plugin work.

## 12. Current next action

Inspect the current branch source around:
- `skyline_trace_probe_detail`
- `skyline_trace_probe`
- `skyline_install_bonus_trace_probes`

Then add a focused temporary disassembly logger for
`libp5r_bdsm.nro+0xdbf0` through `+0xdc40`.

Do not modify unrelated runtime behavior. Review the patch, commit and push
only the intended files, dispatch GitHub Actions, and verify the resulting
build before testing.

After the five-boot analysis, update this handoff with:
- Exact multiplication instruction and operands.
- Store-time register values.
- Memory classification.
- Five-boot distribution.
- Delayed and title-screen reread results.
- Final conclusion and any remaining targeted work.

---

## 13. Untracked files caution

Previously observed untracked files included:

```
android/wonder/importer/build/
android/wonder/importer/nximport.cpp.bak
diagnostics.diff
patch_display_version.py
patch_inline_hook_fix.py
patch_skyline_diagnostics.py
repair_display_version.py
runtime/include/nx/runtime.h.bak-subsdk9
runtime/src/hle_fs.cpp.bak-layout
runtime/src/loader.cpp.bak-subsdk9
runtime/src/main.cpp.bak-exefs-layout
runtime/src/main.cpp.bak-subsdk9
runtime/src/nce/nce.cpp.before-hook-stats
runtime/src/nce/nso.cpp.bak-subsdk9
runtime/src/nce/nso.h.bak-subsdk9
```

This list is historical and may have changed. Always inspect `git status`
before staging. Never stage these files unless explicitly intended.
