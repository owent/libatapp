# libatapp Agent Guide

This is the canonical, cross-agent guide for this subproject. Keep it short: put repeatable workflows in
`.agents/skills/*/SKILL.md`, and keep `.github/copilot-instructions.md` / `CLAUDE.md` as lightweight bridges.

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
- Read the matching `.agents/skills/*/SKILL.md` before build, test, config-expression, connector, module, or etcd work.
- `include/atframe/atapp_conf.proto` is the config source of truth; generated outputs should normally be regenerated,
  not edited by hand.
- After C++ edits, run `clang-format -i <file>` and verify with `clang-format --dry-run --Werror <file>` when practical.

## C++ Conventions

1. **Namespaces**: `atframework::atapp` for library code; `atframework::atapp::protocol` for protobuf types.
2. **Include guards**: use `#pragma once`.
3. **Naming**: classes/functions use `snake_case`; constants use `UPPER_SNAKE_CASE`; type aliases often use `*_t`.
4. **Error handling**: return `int` / `int32_t` error codes (`0` success, negative error).
5. **Logging**: use FWLOG macros (`FWLOGINFO`, `FWLOGERROR`, etc.).
6. **Anonymous namespace + static**: in `.cpp` files, file-local functions should be inside an anonymous namespace **and**
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
- `.github/copilot-instructions.md` exists only to point VS Code Copilot at this guide and `.agents/skills/`.
- `CLAUDE.md` exists only to point Claude-compatible tools at this guide and `.agents/skills/`.
- Keep skill folder names and frontmatter `name` values identical; descriptions are the discovery surface.
