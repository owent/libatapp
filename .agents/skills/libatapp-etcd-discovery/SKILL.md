---
name: libatapp-etcd-discovery
description: "Use when: changing libatapp etcd client, lease, watch, service-discovery, topology, etcd_module lifecycle, node-selection behavior, or etcd-specific tests. Do not use when: only running generic unit tests or editing non-etcd connectors."
---

# etcd Integration and Service Discovery

## Outcome

Keep etcd transport, leases, watchers, discovery indexes, topology, module lifecycle, configuration, and tests consistent
with the current libatapp implementation.

## Route the task

| Area | Source of truth | Load next |
| --- | --- | --- |
| HTTP/KV/watch/lease client | `include/atframe/etcdcli/etcd_cluster.h`, `src/atframe/etcdcli/etcd_cluster.cpp` | [architecture and API](references/architecture-and-api.md) |
| Discovery nodes, indexes, selection | `etcd_discovery.h/.cpp` and discovery tests | [architecture and API](references/architecture-and-api.md) |
| Keepalive, watcher, serialization | `etcd_keepalive.*`, `etcd_watcher.*`, `etcd_packer.*` | [architecture and API](references/architecture-and-api.md) |
| Module lifecycle and topology | `include/atframe/modules/etcd_module.h`, `src/atframe/modules/etcd_module.cpp` | [architecture and API](references/architecture-and-api.md) |
| Configuration contract | `include/atframe/atapp_conf.proto` and current generated/config loader paths | Load the relevant config Skill only if expressions change |
| etcd integration tests | `test/case/atapp_etcd_*`, discovery/topology tests, `ci/etcd/setup-etcd.*` | [test reference](references/testing.md) and `../testing/SKILL.md` |

## Workflow

1. Read the nearest `AGENTS.md`, current diff, relevant headers/implementations, protobuf config, and exact tests before
   proposing behavior. Treat examples in references as navigation, not a substitute for current source.
2. Classify the change: transport/KV, lease/keepalive, watch/revision, discovery selection, topology/module lifecycle,
   serialization, configuration, or tests. Load only the matching reference.
3. Preserve monotonic node versions, consistent-hash and metadata-filter semantics, watcher revision ordering, and
   lifecycle cleanup unless the task explicitly changes their contract.
4. For defects, reproduce the exact path and add or update the narrowest regression test before the fix. For public or
   cross-module behavior changes, define acceptance and rollback before implementation.
5. Use the setup scripts for real-etcd tests; do not silently treat a skipped test as a passing integration test.

## Gotchas

- Tests requiring etcd use `ATAPP_UNIT_TEST_ETCD_HOST`; if it is absent they may skip instead of fail.
- Discovery node versions are monotonic; stale lower-version updates are ignored.
- Use `etcd_packer::get_key_range_end()` for prefix queries; manual range-end logic misses byte edge cases.
- A paused process can lose lease-bound keys, so debugger freezes may look like remote node failure.
- Startup snapshot and watcher handoff must reconcile events without gaps or duplicate state.
- Metadata filters require every non-default rule field to match; an empty filter matches all nodes.

## Validation

- Run the narrow test group for the changed component; use the real-etcd setup only when transport/module behavior needs
  it, and confirm the test executed rather than skipped.
- Run configured formatting/lint checks for touched C++ or protobuf files and `git diff --check` from the libatapp root.
- Re-read config, lifecycle, and discovery contracts after the test pass; do not infer completion from one green filter.
