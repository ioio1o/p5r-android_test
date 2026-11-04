# Android Skyline Plugin Compatibility — Project Status

Last updated: 2026-11-04

Repository: https://github.com/ioio1o/p5r-android_test
Working branch: feature/skyline-plugin-dry-run
Target application: Persona 5 Royal Android NCE port
Target game version: Persona 5 Royal 1.0.2
Title ID: 01005CA01580E000
Expected main build ID prefix: D4B150B29A931CD3

## 1. Project objective

The objective is to implement general Skyline plugin compatibility in the
Android NCE port.

The project is not intended to support only one plugin or a fixed collection
of mods. It aims to provide a reusable runtime environment in which compatible
Skyline plugins can be discovered, loaded, linked, initialized, and executed
without requiring plugin-specific changes to the Android port.

The intended result is an Android application with a predictable plugin
installation structure and runtime behavior comparable in principle to the
plugin-loading experience available in Switch environments.

This is a compatibility effort, not a claim of complete Skyline or Switch
system emulation.

## 2. Testing strategy

Two existing plugins are used as practical testing objects:

- **P5RCBT** — used to exercise plugin discovery, loading, initialization,
  Skyline API calls, hooks, and interaction with the game runtime.
- **BDSM** — used as another real-world test object to exercise plugin
  loading, callback execution, hooks, and runtime behavior.

These plugins are test inputs, not the definition of the project scope.
Their individual features and implementation details do not define the
compatibility architecture.

A fix should be considered part of the general runtime when it addresses a
reusable compatibility requirement. Plugin-specific workarounds should not
replace implementation of the underlying runtime behavior.

Successful execution of either testing object does not by itself establish
general Skyline compatibility. Broader testing with independent plugins
remains necessary.

## 3. Runtime architecture

The Android port uses Native Code Execution (NCE) to execute the game's
AArch64 code on Android.

The existing runtime provides the foundation for the Switch-facing execution
environment, including memory management, kernel functionality, system-call
handling, filesystem services, and graphics integration.

The Skyline compatibility work extends this environment to support additional
plugin modules.

The main areas of responsibility are:

### 3.1 Game and module loading

- Read the game's executable image through the existing game-image and
  importer infrastructure.
- Load and link the main game and SDK modules.
- Establish module address ranges and segment information.
- Resolve relocations and symbols needed by loaded modules.
- Preserve the existing NCE execution model.

### 3.2 Plugin discovery and loading

The current implementation discovers NRO plugins using the plugin directory
under the game's mod filesystem:

`mods/<plugin-package>/romfs/skyline/plugins/*.nro`

The loader is responsible for:

- Finding plugin NRO files.
- Reading and mapping plugin images.
- Preparing module metadata and segment ranges.
- Processing plugin relocations.
- Resolving symbols against the plugin, HLE, game, and SDK environments.
- Preparing plugin modules for NCE execution.
- Running plugin initialization in the configured lifecycle phase.

Plugin loading must remain independent of the identity of a particular plugin.

### 3.3 Module memory and symbol handling

Plugin modules have their own mapped memory and segment information.

The runtime must correctly distinguish:

- Text.
- Read-only data.
- Writable data.
- BSS.
- Main game and SDK module ranges.
- Plugin module ranges.

Address ownership checks must account for all relevant segments. A helper
that recognizes only plugin text must not be treated as proof that an address
belongs to the entire plugin allocation.

Symbol resolution and relocation handling must also work when multiple
independent plugin modules are loaded.

### 3.4 Skyline API and hooks

The compatibility layer provides selected Skyline-facing APIs and hook
mechanisms, including:

- Region address lookup.
- Memory-copy functionality.
- Function hooking.
- Inline hooking.
- Supporting HLE symbol resolution.
- Hook attribution and diagnostic instrumentation.

The hook implementation must preserve the required guest execution context,
including register and stack behavior, and must remain compatible with the
NCE calling conventions.

The current implementation is a subset of Skyline functionality. Full API
coverage and all hook edge cases have not been established.

### 3.5 Filesystem environment

The runtime maps the plugin-facing `sd:` filesystem into the Android
application's user-accessible data environment.

The current mapping is:

`sd:` → `Persona 5 Royal/sdmc`

Plugin discovery and filesystem access should use this shared environment
rather than plugin-specific paths embedded in runtime code.

### 3.6 Initialization lifecycle

Plugin loading and NCE preparation must happen before plugin initialization
routines are invoked.

The implementation supports configurable plugin initialization timing.
The current configuration is maintained under:

`Persona 5 Royal/p5r/`

The initialization order must remain deterministic and must be validated
against constructor and initialization behavior across different plugins.

## 4. Current implementation status

The following status describes the implementation state recorded during
development. It does not imply that every feature has passed broad
compatibility testing.

### Implemented

- Android NCE execution foundation.
- Game and SDK module loading.
- Plugin NRO discovery.
- Plugin module mapping and relocation groundwork.
- Plugin symbol resolution against runtime, game, and SDK symbols.
- Plugin initialization lifecycle integration.
- Skyline-facing region and memory helper functionality.
- Function-hook and inline-hook paths.
- Configurable plugin initialization phase.
- Plugin ownership and hook diagnostic infrastructure.
- Android-side plugin filesystem mapping.
- GitHub Actions APK build workflow.

### Implemented but requiring broader validation

- Compatibility across different Skyline plugins.
- Multiple plugins loaded together.
- Complete plugin segment and memory ownership handling in all cases.
- Relocation edge cases across different NRO layouts.
- Hook trampoline behavior across different callback patterns.
- Threading, synchronization, and TLS behavior expected by plugins.
- Wider Skyline HLE API coverage.
- IPC and system-service behavior required by independent plugins.
- Long-running stability under concurrent plugin activity.

### Not established

- Complete Skyline API compatibility.
- Full behavioral parity with Eden or another Switch emulator.
- Compatibility with every existing Skyline plugin.
- Complete equivalence to the Switch operating-system environment.
- General stability across all Android devices and GPU vendors.

## 5. Diagnostics and engineering approach

Diagnostics are used to identify compatibility gaps in the runtime rather
than to encode the behavior of a particular plugin.

Existing instrumentation covers areas such as:

- Plugin discovery and module loading.
- Relocation and symbol resolution.
- Plugin initialization.
- Hook registration and execution.
- Callback register and memory state.
- HLE calls and selected runtime service interactions.
- Filesystem activity.
- Module address attribution.

Diagnostic instrumentation should be bounded and purposeful. Temporary probes
should be removed or disabled when their investigation is complete.

When investigating a failure:

1. Identify the runtime boundary involved.
2. Capture evidence at that boundary.
3. Determine whether the behavior is a general compatibility requirement.
4. Inspect the corresponding implementation.
5. Make the smallest reusable correction.
6. Validate with the original test case and, where possible, an independent
   plugin.

Do not infer plugin ownership from incomplete address-range checks. Do not
treat an individual successful callback as proof that all related runtime
services are correct.

## 6. Testing and verification

P5RCBT and BDSM are used to exercise different real-world plugin behaviors.
They are complementary test objects, not compatibility targets that should
receive hardcoded special treatment.

Testing should cover the following layers independently:

| Layer | Verification target |
|---|---|
| Discovery | Plugins are found through the documented directory structure. |
| Loading | NRO images are mapped and module metadata is valid. |
| Relocation | Required relocations are processed correctly. |
| Linking | Plugin symbols resolve against the appropriate modules and HLE. |
| Initialization | Constructors and initialization routines execute in the intended order. |
| APIs | Skyline-facing calls reach compatible runtime implementations. |
| Hooks | Function and inline hooks preserve the expected execution context. |
| Filesystem | Plugin filesystem requests resolve through the Android mapping. |
| Multiple plugins | Independent modules can coexist without corrupting each other. |
| Stability | Repeated launches and extended execution do not expose runtime regressions. |

A test result should record the tested application revision, build artifact,
device environment, plugin test object, reproduction procedure, and relevant
logs.

A test passing on one device is evidence for that configuration only.

## 7. Build and development workflow

Development and source changes are made in the repository's feature branch.
The Android APK is built through GitHub Actions; local Android builds are not
part of the current workflow.

The build workflow is:

`.github/workflows/build-apk.yml`

The workflow is manually dispatched. A Git push alone does not establish that
a new APK has been built.

Required change procedure:

1. Inspect the established implementation and current Git state.
2. Make a narrowly scoped change.
3. Review the complete diff.
4. Preserve unrelated changes and untracked files.
5. Stage only the intended files.
6. Commit and push to the active feature branch.
7. Dispatch the GitHub Actions APK workflow.
8. Verify the workflow result and tested commit.
9. Install and test the resulting artifact on the Android device.

Do not claim a build or runtime test succeeded without its corresponding
evidence.

## 8. Repository and upstream status

The current development branch is:

`feature/skyline-plugin-dry-run`

The project is based on the Android port maintained by the upstream repository:

https://github.com/Brandon04Gonzalez/p5r-android

The upstream repository has advanced to version 0.5.5. Synchronization with
that version is pending.

The integration must preserve the custom Android NCE and Skyline work while
incorporating relevant upstream improvements. Particular care is required
where both histories modify the loader, NCE, filesystem, Android platform,
and runtime initialization code.

The planned integration uses a separate Git worktree so that the existing
working checkout, local documentation changes, and untracked investigation
files remain untouched.

The upstream merge must be reviewed and tested before it becomes the new
working baseline.

## 9. Current priorities

### Priority 1 — Preserve and synchronize the implementation

- Record the current Skyline architecture and implementation status.
- Integrate upstream v0.5.5 without losing custom NCE or plugin-loading work.
- Resolve overlapping runtime changes individually.
- Review the resulting source and build it through GitHub Actions.

### Priority 2 — Validate the plugin runtime

- Verify discovery and loading after upstream integration.
- Verify relocations and symbol resolution.
- Verify plugin initialization ordering.
- Verify function and inline hook behavior.
- Verify filesystem mapping and module memory ownership.
- Re-test P5RCBT and BDSM against the integrated build.

### Priority 3 — Improve general compatibility

- Audit missing Skyline HLE APIs.
- Identify required kernel and system-service behavior.
- Improve threading, synchronization, and TLS compatibility.
- Validate memory-management and relocation edge cases.
- Test independent plugins and multiple-plugin configurations.

### Priority 4 — Improve usability

- Maintain a predictable plugin directory structure.
- Provide clear configuration options.
- Report plugin loading and initialization failures clearly.
- Avoid requiring users to modify the APK for individual plugins.

## 10. Compatibility principles

The following principles guide future work:

1. Implement general runtime behavior instead of plugin-name-specific patches.
2. Keep plugin discovery and loading independent of individual plugin identities.
3. Preserve the existing NCE architecture.
4. Treat the Switch-facing runtime environment as a set of explicit compatibility
   responsibilities, not as an assumption of complete operating-system parity.
5. Use real plugins as test objects, not as definitions of the supported API.
6. Separate implemented functionality from verified compatibility.
7. Keep diagnostic instrumentation bounded and remove temporary probes when
   they are no longer needed.
8. Preserve upstream improvements and custom port functionality during merges.
9. Validate changes using the actual Android build and runtime.
10. Expand compatibility incrementally using reproducible evidence.

## 11. Related documentation

- `docs/SKYLINE_COMPATIBILITY_HANDOFF.md` — detailed implementation findings,
  runtime diagnostics, and historical investigation notes.
- `docs/MODS.md` — user-facing filesystem overlay documentation.
- `README.md` — project overview and Android application usage.

This document is the high-level project status and architecture overview.
Detailed diagnostic history and implementation-specific investigation notes
belong in the technical handoff document.
