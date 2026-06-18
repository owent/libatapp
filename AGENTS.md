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
- `.agents/skills/`: build, testing, config-expression, module/connector, etcd/discovery, and AI-agent maintenance
   playbooks.

## Always-On Rules

- Respect the user's dirty workspace: inspect current file contents before editing and avoid unrelated reformatting.
- When creating AI scratch files or asking scripts to emit temporary data/logs, use a subdirectory inside an ignored
   build tree (prefer `build/_agent_tmp/` or an existing `build_jobs_*/_agent_tmp/`) so `.gitignore` covers it; never
   write temporary artifacts to the repository root.
- Before running CMake configure/build/test commands, inspect the active workspace `.vscode/settings.json` when present
  and align with its generator, configure settings, build directory conventions, and parallel-job settings.
- Read the matching `.agents/skills/*/SKILL.md` before build, test, config-expression, connector, module, or etcd work.
- `include/atframe/atapp_conf.proto` is the config source of truth; generated outputs should normally be regenerated,
  not edited by hand.
- After C++ edits, run `clang-format -i <file>` and verify with `clang-format --dry-run --Werror <file>` when practical.

## C++ Conventions

1. **Namespaces**: `atframework::atapp` for library code; `atframework::atapp::protocol` for protobuf types.
2. **Include guards**: use `#pragma once`.
3. **Header code**: any function, method, friend, or operator body written in a header must use
   `ATFW_UTIL_FORCEINLINE`; avoid plain `inline` for project code unless matching generated or third-party code.
4. **Exported ABI**: interfaces declared with `LIBATAPP_MACRO_API` or other `*_API` export macros must be implemented in
   `.cpp` files, not headers, so ABI stays stable across compilers and build options.
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
| `build` | Configuring or building with CMake |
| `testing` | Running or writing private test-framework cases |
| `configure-expression` | Editing env-expression-enabled config fields or syntax |
| `libatapp-module-connector` | Working on modules, connectors, endpoints, routing, or lifecycle hooks |
| `libatapp-etcd-discovery` | Working on etcd client, discovery sets, topology, keepalive, or watchers |
| `ai-agent-maintenance` | Auditing or optimizing AI agent prompts, bridge files, and skills |

## Agent File Compatibility

- `AGENTS.md` is canonical for tools that support hierarchical agent instructions.
- `.agents/skills/` is the portable project skill location; keep each `SKILL.md` focused and self-contained.
- Do not maintain `.github/copilot-instructions.md` copies when `AGENTS.md` and `.agents/skills/` cover the same rules.
- `CLAUDE.md` exists only to point Claude-compatible tools at this guide and `.agents/skills/`.
- Do not make this repository depend on root, sibling, or vendored-submodule prompt files.
- Keep skill folder names and frontmatter `name` values identical; descriptions are the discovery surface.
