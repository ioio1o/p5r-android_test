# Upstream Integration Guidelines

## 1. Objective

Port the verified work from `integration/skyline-android` onto a new branch based on the latest upstream `main`, while preserving upstream improvements and maintaining the existing Skyline and Persona 5 Royal functionality.

Target branch: `integration/skyline-main`

Upstream repository: `Brandon04Gonzalez/p5r-android`

Fork: `ioio1o/p5r-android_test`

Reference branch: `integration/skyline-android`

The reference branch must remain unchanged and available as a fallback and source of comparison.

This is a selective port and adaptation process. It is not a wholesale merge, rebase, or blind cherry-pick operation.

## 2. Non-negotiable principles

1. **No assumptions.** Inspect source code, history, dependencies, and available evidence before making a decision.
2. **Inspect before changing.** Understand the upstream implementation and the existing implementation before modifying a file or function.
3. **Preserve upstream improvements.** Do not replace newer upstream code with older integration-branch code without understanding the differences.
4. **Preserve verified functionality.** Do not silently omit, disable, or regress established features.
5. **Port behavior, not commits.** Cherry-picking is not the default. Adapt functionality to the current upstream architecture.
6. **One logical change at a time.** Keep changes focused and independently inspectable.
7. **Evidence-based decisions.** Record source references, dependencies, test results, and unresolved questions.
8. **No speculative fixes.** If behavior or compatibility is unclear, stop and investigate.
9. **No unrelated changes.** Do not mix cleanup, formatting, refactoring, or unrelated fixes into a feature port.
10. **No unverified completion claims.** Distinguish implementation, build success, runtime validation, and confirmed functionality.

## 3. Branch protection and repository rules

- Perform integration work only on `integration/skyline-main`.
- Keep `integration/skyline-android` unchanged.
- Keep upstream `main` unchanged.
- Do not force-push.
- Do not perform a wholesale merge or rebase of the old integration branch.
- Do not reset, clean, discard, or stash existing work without explicit approval.
- Preserve untracked files and unrelated staged or unstaged changes.
- Never use `git add -A`.
- Stage only explicitly reviewed files.
- Inspect the current branch and working-tree status before every change.
- Do not build locally. Use GitHub Actions for builds.
- Do not claim a push or build succeeded without command or workflow output confirming it.

## 4. Mandatory recommended porting order

Follow these stages in order. Do not skip ahead, mix stages, or repeatedly circle back to completed work without new evidence.

### Stage 1 — NCE and loader foundation

Establish compatibility with the new upstream loader and preserve our NRO/plugin module support.

Inspect and reconcile:

- NCE initialization and execution paths.
- Module loading and unloading.
- NRO parsing and mapping.
- Segment allocation and memory permissions.
- Relocation processing.
- Symbol resolution.
- Branch and trampoline allocation.
- Exception handling and required HLE support.
- Loader interfaces and ownership of module state.
- Dependencies between NCE, the loader, and plugin modules.

Completion requirement: the upstream loader and NCE architecture must support the required module-loading behavior without losing established NRO/plugin functionality.

Do not begin Stage 2 until the foundation and its unresolved dependencies are documented.

### Stage 2 — Skyline runtime

Restore and adapt the Skyline runtime functionality on top of the established foundation.

Inspect and reconcile:

- Plugin discovery and directory scanning.
- Plugin NRO loading and mapping.
- Relocation and symbol linking.
- Skyline HLE implementations.
- Hook registration and execution.
- Inline context and callback behavior.
- Plugin lifecycle and module attribution.
- Filesystem access required by plugins.
- Error handling and compatibility behavior.

Completion requirement: the plugin discovery, loading, linking, HLE, and hook paths must be integrated with the new foundation. Record what is implemented and what still requires runtime validation.

### Stage 3 — Android compatibility

Port useful Android-specific changes after the loader and Skyline runtime are stable.

Inspect and reconcile:

- Android platform integration.
- Filesystem and storage access.
- Game import and content discovery.
- Input and motion handling.
- Resolution and display configuration.
- Fullscreen and stretch options.
- GPU and driver compatibility workarounds.
- Android-specific configuration and environment variables.

For each change, establish whether upstream already provides equivalent behavior. Preserve newer upstream implementations where possible.

Completion requirement: each retained Android-specific change must have a documented purpose, compatibility assessment, and verification status.

### Stage 4 — Rendering and performance

Compare existing rendering and performance changes against upstream's newer NVN implementation before porting anything.

Inspect and reconcile:

- NVN device, queue, and command handling.
- Texture and image handling.
- Descriptor management and caches.
- Synchronization and resource lifetime.
- Rendering workarounds.
- Bloom and other graphics options.
- Performance instrumentation that affects runtime behavior.

Do not assume that an older workaround remains necessary. Verify the relevant upstream code path and establish whether the original problem still exists.

Completion requirement: retain only changes that are still necessary, compatible, and supported by evidence.

### Stage 5 — P5R-specific features

Restore the native aspect patch and other confirmed game-specific improvements.

Inspect and reconcile:

- Persona 5 Royal game-specific patches.
- Native aspect-ratio patch and its configuration.
- Game initialization and patch timing.
- Version and instruction validation.
- Other confirmed game-specific improvements.

Preserve original author attribution and licensing notices where applicable. Do not port speculative game-specific patches.

Completion requirement: each retained patch must have documented target conditions, safety checks, configuration behavior, and verification status.

### Stage 6 — Diagnostics and documentation

Retain useful permanent diagnostics and remove obsolete investigation instrumentation.

Inspect and reconcile:

- Permanent error and compatibility reporting.
- Plugin and module attribution.
- Hook and callback diagnostics.
- Memory and runtime instrumentation.
- Temporary tracing and investigation-only code.
- Project status and compatibility documentation.
- Build and testing instructions.

Do not remove a diagnostic merely because it was added during an investigation. First establish whether it still provides useful operational information.

Completion requirement: permanent diagnostics are intentional and documented; obsolete investigation-only instrumentation is identified and removed only after its purpose and dependencies are understood.

## 5. Strict sequencing rules

- Complete the current stage before starting the next.
- Maintain a clear stage status: not started, inspecting, porting, verifying, blocked, or complete.
- Resolve or explicitly document foundation issues before proceeding.
- Do not revisit a completed stage unless new evidence identifies a regression, missing dependency, or incorrect classification.
- Before reopening a completed stage, record the new evidence, reason, and affected features.
- Keep a record of stage boundaries, completed features, outstanding work, and known limitations.
- Do not use later-stage work as a reason to bypass an unresolved earlier-stage dependency.

## 6. Required inspection procedure

For every feature or group of related changes:

1. Identify the original implementation on `integration/skyline-android`.
2. Identify the corresponding current upstream files, functions, and interfaces.
3. Inspect the complete relevant implementations, not just commit messages or filenames.
4. Inspect relevant history and dependencies.
5. Compare behavior, architecture, and assumptions.
6. Determine whether upstream already implements equivalent functionality.
7. Identify upstream changes that would be overwritten or affected by a proposed port.
8. Record the intended adaptation and its dependencies.
9. Make the smallest coherent change.
10. Inspect the complete diff.
11. Verify the change using the available build or runtime evidence.
12. Update the integration inventory and documentation.

If the relevant implementation or its behavior cannot be established, mark it unresolved and stop work on dependent changes.

## 7. Feature classification

Every identified feature must receive one of these classifications:

- **Already upstream** — Equivalent functionality exists in current upstream code. Do not duplicate it.
- **Port required** — Functionality is absent and remains necessary.
- **Adapt required** — Functionality is necessary, but upstream architecture or interfaces require a new implementation.
- **Superseded** — A newer upstream implementation replaces the old behavior.
- **Experimental** — The change is not sufficiently verified for normal integration.
- **Obsolete** — Evidence establishes that the change is no longer needed.
- **Unresolved** — Available evidence is insufficient to make a safe decision.

Do not classify a feature as obsolete or superseded merely because its original code differs from upstream.

## 8. Implementation and conflict resolution

- Prefer adapting the smallest relevant function or component.
- Preserve upstream interfaces and newer behavior unless a documented requirement proves a change is necessary.
- Avoid copying entire files when only a limited behavior is needed.
- Inspect callers, callees, shared structures, build configuration, and platform-specific paths affected by a change.
- Resolve conflicts according to behavior and architecture, not commit age or line count.
- Keep feature dependencies explicit.
- Do not combine unrelated features to make a conflict easier to resolve.
- If two implementations cannot be reconciled confidently, stop and document the conflict.

## 9. Verification and build policy

Local builds are prohibited for this project. Use GitHub Actions for APK builds.

For each logical port:

1. Inspect the complete source diff.
2. Verify affected call paths and dependencies.
3. Run available static or repository checks that do not constitute a local build.
4. Commit and push only after the change has been reviewed.
5. Trigger the appropriate GitHub Actions workflow.
6. Inspect the workflow result and artifacts.
7. Perform device/runtime validation when the feature requires it.
8. Record exactly what was and was not verified.

A successful build does not prove runtime correctness. A successful launch does not prove every feature works.

Never report a feature as runtime-verified without corresponding device evidence.

## 10. Git and Termux operating rules

Every Termux command must begin with:

`cd ~/p5r-android_skyline-integration || exit 1`

- Do not use `/tmp` or Termux temporary directories.
- Do not build locally.
- Inspect `git status` before changing files.
- Preserve unrelated staged, unstaged, and untracked files.
- Never use `git add -A`.
- Stage only named files that have been reviewed.
- Never reset, clean, discard, or stash without explicit approval.
- Inspect full diffs without truncation:

  `git --no-pager diff --no-ext-diff --unified=12`

- Inspect staged changes separately before committing.
- Do not amend or rewrite published commits without explicit approval.
- Do not force-push.
- Do not claim a commit or push succeeded without its command output.

## 11. Diagnostics and experimental code policy

- Separate permanent diagnostics from temporary investigation instrumentation.
- Keep useful diagnostics that support compatibility, error reporting, and plugin attribution.
- Identify temporary instrumentation by purpose and evidence before proposing removal.
- Do not remove diagnostics simply to reduce logging.
- Do not retain invasive or performance-affecting instrumentation without a documented reason.
- Keep experimental changes out of the verified feature set until evidence supports them.

## 12. Documentation requirements

Maintain the following records as integration progresses:

- This guideline.
- `docs/PROJECT_STATUS.md`.
- `docs/SKYLINE_COMPATIBILITY_HANDOFF.md`.
- A feature integration inventory.

Update documentation when implementation status changes. Clearly distinguish implemented, built, device-tested, runtime-verified, experimental, and unresolved behavior.

Do not copy stale branch names, commit references, or workflow instructions into updated documentation without verifying them against the current repository.

## 13. Feature integration inventory

Track each feature with at least these fields:

| Field | Required information |
|---|---|
| Stage | One of the six mandatory stages |
| Feature | Specific behavior being evaluated |
| Original source | File, function, and reference branch location |
| Upstream source | Corresponding file, function, and upstream reference |
| Classification | One of the defined feature classifications |
| Dependencies | Required functions, modules, interfaces, or configuration |
| Port plan | Required adaptation or reason no port is needed |
| Implementation status | Not started, in progress, blocked, or complete |
| Build status | Not run, pending, passed, or failed |
| Runtime status | Not tested, pending, passed, failed, or not applicable |
| Evidence | Commit, diff, logs, workflow run, or device observations |
| Notes | Limitations, conflicts, and unresolved questions |

Do not mark a feature complete based only on code being present.

## 14. Stage completion criteria

A stage is complete only when:

- Its relevant features have been inspected and classified.
- Required functionality has been ported or adapted.
- Upstream behavior has been preserved where appropriate.
- Dependencies and conflicts have been resolved or explicitly documented.
- The complete diff has been reviewed.
- Required build and runtime verification has been recorded.
- The integration inventory and relevant documentation are updated.
- No unrecorded work remains in that stage.

A stage may be marked blocked when evidence or a dependency prevents safe progress. A blocked stage does not authorize skipping ahead.

## 15. Final integration completion criteria

The integration is complete only when:

- All six stages have been completed in the prescribed order.
- Every identified feature has a classification and recorded disposition.
- Required functionality from the reference branch has been restored or its omission justified with evidence.
- Upstream improvements have been preserved.
- No unresolved integration conflict is silently ignored.
- GitHub Actions build results are recorded.
- Required device/runtime validation is recorded.
- Documentation accurately describes the resulting implementation and limitations.
- The final branch diff and commit history have been reviewed.

## 16. Governing rule

When source code, dependencies, compatibility, or behavior is unclear, stop and investigate.

Do not guess, silently omit functionality, skip stages, or proceed on an unverified assumption.

Follow the six-stage porting order strictly. Reopen completed stages only when new evidence justifies it, and document that evidence before making changes.
