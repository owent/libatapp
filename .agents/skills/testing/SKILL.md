---
name: testing
description: "Use when: designing, writing, reviewing, or running libatapp unit tests, filtering private-framework cases, testing multi-app/synthetic-time behavior, selecting etcd integration tests, or diagnosing Windows test startup/PATH."
---

# Unit testing (libatapp)

The test executable is `atapp_unit_test`; current cases and their real fixtures/helpers live under `test/case/`.

Read [test design and acceptance](references/test-design-and-acceptance.md) when planning, writing, or reviewing cases.
It is not needed merely to run a known test command. For etcd/discovery-specific behavior and setup, also use
`libatapp-etcd-discovery` and its [test reference](../libatapp-etcd-discovery/references/testing.md).

## Choose the narrowest real path

- Inspect the closest current test source and reuse its actual fixture/helper; do not invent a generic `pump_until`,
  mock API, config field, discovery field, or cleanup method in guidance or code.
- Test configuration loading, packers, discovery selection/versioning, module helpers, connectors, and other in-process
  logic through real public/generated types without starting etcd or a network when those dependencies are not the
  behavior under test.
- For multi-app routing/topology/lifecycle behavior, use the existing in-process app/atbus setup and assert externally
  observable messages, endpoint/connector state, callbacks, errors, and cleanup. Do not bypass the layer named by the
  case or assert only that a fake was called.
- Build protobuf and YAML/config data from current schemas plus a nearby contract-valid fixture. Keep only verified
  prerequisites and behavior-relevant fields; do not inflate idle timeouts, retries, labels, or readiness values merely
  to prevent the current flow from failing.

## Drive time and I/O deterministically

- Use `atframework::atapp::app::set_sys_now(...)` only in Debug builds (`!NDEBUG`), where the current header exposes it,
  and only when timer behavior is the subject. It changes process-global app time: keep affected cases serial, avoid
  cross-app interference, and restore the real project clock on every exit path.
- Advance mock time and call the relevant `tick()`/event pump until a named state transition occurs. Do not use a fixed
  sleep, real elapsed time, CPU/network jitter, or a precise number of pump iterations as the oracle.
- When real loopback I/O is the subject, use the nearest predicate-driven event-loop helper. A short yield/sleep may let
  libuv progress, and a wall timeout may prevent a hang, but correctness must be proved by an explicit final predicate
  and business-result assertion.
- Keep process-global callbacks, discovery/module state, app instances, sockets, and temporary config isolated and
  cleaned up. `CASE_EXPECT_*` is non-fatal, so guard dependent operations after a failed setup assertion.

## Etcd boundary

`atapp_etcd_cluster` and `atapp_etcd_module` are integration groups. The current source uses
`ATAPP_UNIT_TEST_ETCD_HOST` when set and otherwise probes `http://127.0.0.1:12379`; lack of an available endpoint causes
the case path to return without exercising etcd. Confirm output/case behavior and report it as unavailable/skipped
integration coverage, never as a passing unit substitute. Prefer the in-process discovery/packer/module-unit groups when
real etcd is not the subject.

## Run tests

Resolve `<BUILD_DIR>` as required by `AGENTS.md`, then prefer the registered CTest:

```bash
ctest --test-dir <BUILD_DIR> -R "^libatapp\.unit_test$" --output-on-failure
```

Add `-C <CONFIG>` for a verified multi-config generator.

The executable supports:

- List: `atapp_unit_test -l` / `--list-tests`
- Run a group/case: `atapp_unit_test -r <group>` or `-r <group>.<case>`
- Filter: `atapp_unit_test -f "pattern*"` / `--filter "pattern*"`
- Help/version: `-h`, `-v`

Run the exact case first and confirm it was selected, then the registered CTest and broader coverage in proportion to
risk. Discover current groups with `-l` and source search; do not maintain hand-counted test tables in this Skill.

## Windows startup

Prefer the registered CTest command so the target and working directory match current CMake configuration. If CTest or a
direct run reports missing DLLs, locate the actual executable/DLL outputs in the current build tree and prepend only
those verified directories to the current process `PATH`. Do not hardcode a parent checkout's `_deps` layout or a
third-party install triplet the current build did not produce.
