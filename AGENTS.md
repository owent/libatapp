# libatapp Agent Guide

This is the canonical, self-contained cross-agent guide for this repository. Keep it short: put repeatable workflows in
`.agents/skills/*/SKILL.md`, keep `CLAUDE.md` as a lightweight bridge, and avoid redundant tool-specific prompt copies.
This repository manages its own AI agent prompts and skills; it must not depend on a parent or sibling repository guide.

**libatapp** is a high-performance asynchronous application framework with configuration loading, modules, service
discovery, connector-based routing, worker pools, and libatbus integration.

- **Repository**: <https://github.com/atframework/libatapp>
- **Languages**: C++ (C++17 required, C++17/C++20/C++23 features used when available)

## Project Map

- `include/atframe/`: public application, config, module, connector, endpoint, etcd, and worker APIs.
- `src/atframe/`: app lifecycle, configuration, connectors, etcd client/discovery, modules, and logging.
- `test/case/`: private unit tests, multi-node topology tests, etcd tests, and config-loader tests.
- `sample/`, `binding/`, `tools/`: examples, language bindings, and utilities.
- `.agents/skills/`: engineering, build, testing, config-expression, module/connector, etcd/discovery, and AI maintenance
   playbooks.

## Always-On Rules

- Respect the user's dirty workspace: inspect current file contents before editing and avoid unrelated reformatting.
- Start with the current task, nearest instructions, Skill index, and capabilities actually exposed by the active agent
  harness. Load full Skill bodies or tool-specific directories only when the task routes there; do not assume or install
  absent workflows, tools, modes, or extensions.
- Before a nontrivial plan or edit, inspect the relevant code, configs, docs, generated sources, tests, and current
  official docs for mutable external behavior. Separate verified facts from assumptions, then state the smallest plan
  and verification path.
- For test work, derive cases from verified behavior and risk. Exercise a contract-valid workflow with deterministic
  controls, then cover the highest-value boundaries and failures. Do not invent interfaces, fields, environments, or
  root causes; do not shape data only to make a test pass or add redundant cases for counts. When a check fails, fix
  the implementation or the case design; never weaken, skip, or loosen an existing assertion, add a retry, or widen a
  timeout merely to force green. If scope or prerequisites remain unclear, state assumptions and coverage gaps instead
  of claiming completeness.
- Match process to risk: use the shortest verified path for small changes; read `change-workflow` for defects and for
  cross-module behavior, public API/ABI, data model/migration, security, or deployment changes. Keep their scope and
  acceptance in one existing authoritative artifact or active task plan; do not initialize a methodology for ceremony.
- Never unconditionally `touch` or overwrite code/resources consumed by `add_custom_command`, `add_custom_target`,
  `add_executable`, `add_library`, or `target_sources`, including generated, copied, and other non-handwritten files.
  Preserve timestamps when content is unchanged; declare real `OUTPUT`/`BYPRODUCTS` and accurate `DEPENDS`/`DEPFILE`,
  and use content-stable generation or a temporary file plus `cmake -E copy_if_different`.
- Resolve `<BUILD_DIR>` before creating build trees or temporary files: prefer the nearest `.vscode/settings.json`
  `cmake.buildDirectory`; otherwise use clangd `--compile-commands-dir=...`, an existing configured tree, then `build`.
- Put AI scratch files and script/log output under `<BUILD_DIR>/_agent_tmp/...`, never in the repository root.
- Read the matching `.agents/skills/*/SKILL.md` before C++ edit/review, build, test, config, connector, module, or etcd work.
- `include/atframe/atapp_conf.proto` is the config source of truth; generated outputs should normally be regenerated,
  not edited by hand.
- After C++ edits, run `clang-format -i <file>` and verify with `clang-format --dry-run --Werror <file>` when practical.

## Terminal and Tooling

- On Windows use PowerShell 7+ (`pwsh.exe`); never use the legacy Windows PowerShell 5.1 (`powershell.exe`). Launch
  independent processes with `pwsh.exe -NoLogo -NoProfile`, and do not nest `cmd.exe`, Git Bash, WSL, or other shells
  unless the task explicitly requires it.
- Probe before use (`Get-Command <name>`): prefer an installed modern CLI tool (`rg`, `fd`, `sd`, `bat`, `jq`, ...)
  and fall back to PowerShell cmdlets or the traditional tool when it is absent.
- Write PowerShell defensively: single quotes unless expansion is needed, `${name}` for ambiguous boundaries,
  here-strings instead of heredocs, `& { ... } | ...` to pipe statement blocks, and full cmdlet names instead of
  ambiguous Unix aliases (`cat`, `find`, `where`).
- Read the `shell-tooling` skill for the full modern-tool inventory, install channels, agent practices, and
  PowerShell authoring rules before shell-heavy work or command-failure debugging.

## C++ Conventions

1. **Namespaces**: `atframework::atapp` for library code; `atframework::atapp::protocol` for protobuf types.
2. **Include guards**: use `#pragma once`.
3. **Header/API visibility**: public non-template APIs must use `LIBATAPP_MACRO_API` (or the matching `*_API` macro) or
   `ATFW_UTIL_FORCEINLINE`; public template functions defined in headers may use `ATFW_UTIL_SYMBOL_VISIBLE`. Read
   `engineering-guidelines` for the ODR and internal-only rules.
4. **Exported ABI**: keep non-template implementations covered by `LIBATAPP_MACRO_API` or another `*_API` macro in
   `.cpp` files so ABI stays stable across compilers and build options.
5. **Naming**: classes/functions use `snake_case`; constants use `UPPER_SNAKE_CASE`; type aliases often use `*_t`.
6. **Error handling**: return `int` / `int32_t` error codes (`0` success, negative error).
7. **Logging**: use FWLOG macros (`FWLOGINFO`, `FWLOGERROR`, etc.).
8. **Anonymous namespace + static**: in `.cpp` files, file-local functions should be inside an anonymous namespace **and**
   keep the `static` keyword.

   ```cpp
   namespace {
   static void my_helper() { /* ... */ }
   }  // namespace
   ```

## Skill Routing

Read the matching `.agents/skills/*/SKILL.md` before specialized work:

| Skill | Use when |
| --- | --- |
| `engineering-guidelines` | Writing/reviewing C++, header template visibility, or exported API ABI |
| `change-workflow` | Diagnosing defects or delivering nontrivial/high-risk changes with a reviewable contract |
| `build` | Configuring or building with CMake |
| `testing` | Running or writing private test-framework cases |
| `configure-expression` | Editing env-expression-enabled config fields or syntax |
| `libatapp-module-connector` | Working on modules, connectors, endpoints, routing, or lifecycle hooks |
| `libatapp-etcd-discovery` | Working on etcd client, discovery sets, topology, keepalive, or watchers |
| `ai-agent-maintenance` | Auditing or optimizing AI agent prompts, bridge files, and skills |
| `shell-tooling` | Running terminal commands, choosing modern CLI tools, or writing/debugging PowerShell |

## Agent File Compatibility

- `AGENTS.md` is canonical for tools that support hierarchical agent instructions.
- `.agents/skills/` is the portable project skill location; keep each `SKILL.md` focused and self-contained.
- Do not maintain `.github/copilot-instructions.md` copies when `AGENTS.md` and `.agents/skills/` cover the same rules.
- `CLAUDE.md` exists only to point Claude-compatible tools at this guide and `.agents/skills/`.
- Do not make this repository depend on root, sibling, or vendored-submodule prompt files.
- Keep skill folder names and frontmatter `name` values identical; descriptions are the discovery surface.
