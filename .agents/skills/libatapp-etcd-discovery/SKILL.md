---
name: libatapp-etcd-discovery
description: "Use when: working on libatapp etcd integration, service discovery sets, topology management, keepalive actors, watchers, node selection, or etcd_module."
---

# etcd Integration & Service Discovery (libatapp)

## Overview

libatapp integrates with etcd v3 for:

- **Service discovery**: Nodes register themselves and discover others via etcd key-value store
- **Topology management**: Track the bus topology of all nodes in the cluster
- **Keepalive**: Maintain etcd lease-bound keys for presence detection
- **Watch**: React to node join/leave events in real time

## Architecture

```
┌─────────────────────────────────────────────────┐
│  etcd_module (module_impl)                      │
│    ├── Discovery keepalive (register self)      │
│    ├── Topology keepalive (register topology)   │
│    ├── Discovery watcher (watch others)         │
│    ├── Topology watcher (watch topology)        │
│    ├── Snapshot events (initial state sync)     │
│    └── Node event callbacks                     │
├─────────────────────────────────────────────────┤
│  etcd_discovery_set                             │
│    ├── Node index by id / name                  │
│    ├── Consistent hash ring                     │
│    ├── Round-robin counter                      │
│    └── Metadata filter                          │
├─────────────────────────────────────────────────┤
│  etcd_discovery_node                            │
│    ├── Node info (id, name, hostname, pid)      │
│    ├── Version tracking                         │
│    └── Gateway address rotation                 │
├─────────────────────────────────────────────────┤
│  etcd_cluster (HTTP client)                     │
│    ├── KV get/set/del                           │
│    ├── Watch (long-poll)                        │
│    ├── Lease grant/renew/revoke                 │
│    ├── Keepalive management                     │
│    ├── Watcher management                       │
│    └── Stats tracking                           │
├─────────────────────────────────────────────────┤
│  etcd_keepalive                                 │
│    └── Periodic set with lease + checker fn     │
├─────────────────────────────────────────────────┤
│  etcd_watcher                                   │
│    └── Key range watch + revision tracking      │
├─────────────────────────────────────────────────┤
│  etcd_packer                                    │
│    └── JSON ⇄ proto, base64, key range prefix   │
└─────────────────────────────────────────────────┘
```

## etcd Cluster Client (`etcd_cluster`)

### Initialization

```cpp
auto curl_mgr = atfw::util::network::http_request::create_curl_multi(...);
etcd_cluster cluster;
cluster.init(curl_mgr);

// Configure endpoints
cluster.set_conf_hosts({"http://127.0.0.1:2379"});

// Optional TLS
cluster.set_conf_ssl_client_cert(cert_path);
cluster.set_conf_ssl_client_key(key_path);
cluster.set_conf_ssl_ca_cert(ca_path);
```

### KV Operations

```cpp
// GET
auto req = cluster.create_request_kv_get(key);
auto req = cluster.create_request_kv_get(key, range_end);  // Range query

// SET (with optional lease binding)
auto req = cluster.create_request_kv_set(key, value, /*assign_lease=*/true);

// DELETE
auto req = cluster.create_request_kv_del(key);
auto req = cluster.create_request_kv_del(key, range_end);  // Range delete
```

### Watch

```cpp
auto req = cluster.create_request_watch(
    key,
    range_end,         // "" for single key
    start_revision,    // 0 for current
    prev_kv,           // Include previous KV in events
    progress_notify    // Send progress notifications
);
```

### Lease

```cpp
int64_t lease_id = cluster.get_lease();  // Get current lease
// Lease is auto-granted and renewed by the cluster
```

### Stats

```cpp
const auto &stats = cluster.get_stats();
stats.sum_error_requests;
stats.continue_error_requests;
stats.sum_success_requests;
stats.continue_success_requests;
stats.sum_create_requests;
```

### Cluster Events

```cpp
// Notify when etcd becomes available
auto handle = cluster.add_on_event_up([](etcd_cluster &) {
    // etcd is reachable
}, /*trigger_if_running=*/true);

// Notify when etcd becomes unavailable
auto handle = cluster.add_on_event_down([](etcd_cluster &) {
    // etcd connection lost
});

// Clean up
cluster.remove_on_event_up(handle);
```

### Lifecycle

```cpp
cluster.tick();                    // Process pending requests (call in tick loop)
cluster.is_available();            // Is etcd reachable?
cluster.close(wait, revoke_lease); // Shutdown
cluster.reset();                   // Full reset
```

## Discovery Node (`etcd_discovery_node`)

Represents a single discovered service node:

```cpp
auto node = std::make_shared<etcd_discovery_node>();

// Set node info (typically from etcd watch event)
node->copy_from(node_info_protobuf);

// Access fields
node->get_discovery_info().id();
node->get_discovery_info().name();
node->get_discovery_info().hostname();
node->get_discovery_info().pid();

// Version tracking (monotonic)
// Used to detect stale updates — newer version always wins
node->get_version();

// Gateway address rotation
// Returns next gateway address in round-robin for client connections
node->next_gateway_addr();
```

## Discovery Set (`etcd_discovery_set`)

A collection of discovery nodes with multiple selection strategies.

### Node Selection

```cpp
etcd_discovery_set &discovery = etcd_module->get_global_discovery();

// By ID or name
auto node = discovery.get_node_by_id(0x12345678);
auto node = discovery.get_node_by_name("my_service");

// Load balancing
auto node = discovery.get_node_by_consistent_hash(hash_key);  // uint64, int64, string, span
auto node = discovery.get_node_by_round_robin();               // Sequential rotation
auto node = discovery.get_node_by_random();                    // Random pick

// All methods accept optional metadata filter
atapp::protocol::atapp_metadata filter;
filter.set_area_id(1001);
auto node = discovery.get_node_by_round_robin(&filter);
```

### Consistent Hash

The discovery set maintains a **consistent hash ring** for stable key→node mapping:

- Nodes are placed on the ring at multiple virtual positions
- `get_node_by_consistent_hash(key)` finds the nearest node clockwise from the hash of `key`
- Adding/removing nodes only redistributes keys near the affected positions

```cpp
// Lower bound query (for multi-node operations)
std::vector<node_hash_type> output(3);
size_t count = discovery.lower_bound_node_hash_by_consistent_hash(
    output, key_hash, &metadata_filter
);
```

### Sorted Node Access

```cpp
const auto &sorted = discovery.get_sorted_nodes();  // Sorted by (id, name)
auto it = discovery.lower_bound_sorted_nodes(id, name);
auto it = discovery.upper_bound_sorted_nodes(id, name);
```

### Metadata Filtering

All selection methods accept `const metadata_type *metadata` for filtering:

```cpp
// Static filter function
bool match = metadata_equal_type::filter(rule_metadata, node_metadata);
```

The filter checks specific fields in `atapp::protocol::atapp_metadata` (e.g., area_id, region).

### Node Management

```cpp
discovery.add_node(node_ptr);
discovery.remove_node(node_ptr);
discovery.remove_node(id);
discovery.remove_node(name);
discovery.empty();
```

## etcd Keepalive (`etcd_keepalive`)

Binds a key to the cluster's lease and periodically refreshes its value:

```cpp
auto keepalive = std::make_shared<etcd_keepalive>(cluster, etcd_path_key);

// Set value to keep alive
keepalive->set_value(serialized_node_info);

// Set checker function (called to verify value is still valid)
keepalive->set_checker(checker_fn);

// Register with cluster
cluster.add_keepalive(keepalive);

// Remove when done
cluster.remove_keepalive(keepalive);
```

The keepalive:

1. Sets the key with the cluster's lease (auto-deleted if lease expires)
2. Periodically re-sets the value (to handle etcd compaction or restarts)
3. Calls checker function before each set to verify the value

## etcd Watcher (`etcd_watcher`)

Watches a key range for put/delete events:

```cpp
auto watcher = std::make_shared<etcd_watcher>(
    cluster, key_prefix, range_end, event_callback
);

// Event callback receives:
//   etcd_discovery_action_t::kPut    — key created/updated
//   etcd_discovery_action_t::kDelete — key deleted

// Register with cluster
cluster.add_watcher(watcher);

// Remove when done
cluster.remove_watcher(watcher);
```

The watcher:

1. Creates a long-poll watch request
2. Tracks etcd revision for consistency
3. Fires callbacks for each event in order
4. Re-establishes watch if connection drops

## etcd Module (`etcd_module`)

The `etcd_module` integrates etcd with the app lifecycle as a `module_impl`.

### Configuration

```yaml
etcd:
  hosts:
    - "http://127.0.0.1:2379"
  path: "/atapp/services/my_cluster"
  authorization: "" # Optional auth token
  ssl:
    enable: false
    cert: ""
    key: ""
    ca_cert: ""
```

### Module Lifecycle

```
init()    → Create etcd_cluster, set up discovery/topology keepalives and watchers
tick()    → cluster.tick(), process pending watchers/keepalives
reload()  → Update config, reconnect if hosts changed
stop()    → Close watchers, revoke keepalives
timeout() → Force close
```

### Discovery & Topology

The module manages two data sets:

| Set              | Storage                  | Key Pattern            | Purpose                 |
| ---------------- | ------------------------ | ---------------------- | ----------------------- |
| Global Discovery | `etcd_discovery_set`     | `{path}/by_id/{id}`    | All known service nodes |
| Topology Info    | `unordered_map<id, ...>` | `{path}/topology/{id}` | Bus topology of nodes   |

### Key APIs

```cpp
// Access discovery set
etcd_discovery_set &disc = etcd_module->get_global_discovery();

// Access topology info
const auto &topo = etcd_module->get_topology_info_set();

// Subscribe to node events
auto handle = etcd_module->add_on_node_discovery_event(
    [](etcd_discovery_action_t action, const etcd_discovery_node::ptr_t &node) {
        if (action == etcd_discovery_action_t::kPut) {
            // Node joined or updated
        } else if (action == etcd_discovery_action_t::kDelete) {
            // Node left
        }
    }
);
etcd_module->remove_on_node_event(handle);

// Subscribe to topology events
auto handle = etcd_module->add_on_topology_info_event(topology_callback);
etcd_module->remove_on_topology_info_event(handle);
```

### Keepalive Actors

The module provides a simplified API for custom keepalive registrations:

```cpp
// Register a custom keepalive (value is serialized node info)
std::string value = serialize_my_data();
auto keepalive = etcd_module->add_keepalive_actor(value, custom_etcd_path);

// Remove when done
etcd_module->remove_keepalive_actor(keepalive);
```

### Update Notifications

When local node state changes, notify the module to refresh keepalive values:

```cpp
etcd_module->set_maybe_update_keepalive_discovery_value();     // Node info changed
etcd_module->set_maybe_update_keepalive_topology_value();      // Topology changed
etcd_module->set_maybe_update_keepalive_discovery_area();      // Area/region changed
etcd_module->set_maybe_update_keepalive_discovery_metadata();  // Metadata changed
```

### Snapshot Events

For initial state synchronization on startup:

```cpp
// Called when loading snapshot from etcd
auto handle = etcd_module->add_on_load_discovery_snapshot(callback);

// Called when snapshot loading is complete
auto handle = etcd_module->add_on_discovery_snapshot_loaded(callback);

// Check if snapshot is available
bool has = etcd_module->has_discovery_snapshot();
```

### Raw etcd Access

```cpp
// For custom etcd operations not covered by the module
etcd_cluster &cluster = etcd_module->get_raw_etcd_ctx();
auto req = cluster.create_request_kv_get("/my/custom/key");
```

### etcd Path Queries

```cpp
etcd_module->get_discovery_by_id_path();       // e.g., "/atapp/services/cluster/by_id/"
etcd_module->get_discovery_by_name_path();     // e.g., "/atapp/services/cluster/by_name/"
etcd_module->get_topology_path();              // e.g., "/atapp/services/cluster/topology/"
```

## etcd Packer (`etcd_packer`)

Utility functions for etcd data serialization:

```cpp
// Pack/unpack protobuf ↔ JSON (via RapidJSON)
etcd_packer::pack(const google::protobuf::Message &msg, std::string &output);
etcd_packer::unpack(const std::string &input, google::protobuf::Message &msg);

// Key range prefix (for etcd range queries)
etcd_packer::get_key_range_end(const std::string &key);  // "abc" → "abd"

// Base64 encoding (etcd values are base64-encoded)
etcd_packer::base64_encode(input, output);
etcd_packer::base64_decode(input, output);

// Integer format parsing
etcd_packer::parse_int(string_view, int64_t &);
```

## Testing etcd Integration

### Tests Requiring etcd

Tests in `atapp_etcd_cluster_test.cpp` and `atapp_etcd_module_test.cpp` require a running etcd instance.

#### Quick Start with setup-etcd Scripts

Use the `ci/etcd/setup-etcd` scripts to download and start a local etcd:

```bash
# Linux / macOS
bash ci/etcd/setup-etcd.sh start                              # Download (if needed) + start
export ATAPP_UNIT_TEST_ETCD_HOST="http://127.0.0.1:12379"      # Default client port is 12379
./atapp_unit_test -r atapp_etcd_cluster
./atapp_unit_test -r atapp_etcd_module
bash ci/etcd/setup-etcd.sh stop
```

```powershell
# Windows (PowerShell)
.\ci\etcd\setup-etcd.ps1 -Command start
$env:ATAPP_UNIT_TEST_ETCD_HOST = "http://127.0.0.1:12379"
./atapp_unit_test.exe -r atapp_etcd_cluster
./atapp_unit_test.exe -r atapp_etcd_module
.\ci\etcd\setup-etcd.ps1 -Command stop
```

Other commands: `download` (download only), `cleanup` (stop + delete), `status` (check health).
Options: `--work-dir DIR`, `--client-port PORT`, `--peer-port PORT`, `--etcd-version VER`.

#### Manual etcd Setup

If you already have etcd running elsewhere:

```bash
export ATAPP_UNIT_TEST_ETCD_HOST="http://127.0.0.1:2379"
./atapp_unit_test -r atapp_etcd_cluster
./atapp_unit_test -r atapp_etcd_module
```

If `ATAPP_UNIT_TEST_ETCD_HOST` is not set, these tests are skipped (not failed).

### Tests Not Requiring etcd

Discovery set and packer tests work without etcd:

```bash
./atapp_unit_test -r atapp_discovery
./atapp_unit_test -r atapp_etcd_packer
```

### Mock Discovery in Tests

For tests that don't use a real etcd, inject discovery nodes directly:

```cpp
// Create a discovery node from protobuf
atapp::protocol::atapp_discovery node_info;
node_info.set_id(0x201);
node_info.set_name("test_node");
node_info.set_hostname("localhost");
// ... set listen addresses, metadata, etc.

auto node = std::make_shared<etcd_discovery_node>();
node->copy_from(node_info);

// Inject into the global discovery set
etcd_module->get_global_discovery().add_node(node);

// Trigger connection via connector
// The app's message routing will find the node and attempt connection
```

### Discovery Set Unit Tests

```bash
# Key test cases:
#   metadata_filter           — filter nodes by metadata
#   get_discovery_by_metadata — select with metadata
#   round_robin               — sequential rotation
#   lower_bound_*             — hash ring queries (normal, unique, compact)
#   discovery_node_version_update  — version ordering
#   add_remove_stress         — 200-node add/remove
#   ingress_round_robin       — ingress path rotation
#   empty_set_operations      — edge cases on empty set
./atapp_unit_test -r atapp_discovery
```

## Common Pitfalls

1. **etcd not available**: Tests that require etcd check `ATAPP_UNIT_TEST_ETCD_HOST`. If not set, tests are skipped (not failed).

2. **Stale discovery nodes**: Discovery nodes have monotonic versions. If an update arrives with a lower version, it's ignored. Use `discovery_node_version_update` test as reference.

3. **Key range prefix**: Use `etcd_packer::get_key_range_end()` to compute the correct range end for prefix queries. Don't manually compute it — edge cases with `\xff` bytes exist.

4. **Lease expiry**: If the app stops refreshing the lease (e.g., frozen by debugger), etcd deletes all lease-bound keys. Other nodes will see the node as offline.

5. **Snapshot timing**: On startup, the module loads a snapshot of all existing discovery nodes before starting the watcher. Events between snapshot load and watcher start are reconciled automatically.

6. **Metadata filter semantics**: The filter requires ALL non-default fields in the rule to match. An empty filter matches everything.
