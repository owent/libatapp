# libatapp - Copilot Instructions

## Project Overview

**libatapp** is a high-performance, fully asynchronous cross-platform application framework library. It provides a standardized way to build server applications with support for configuration management, module systems, service discovery (etcd), connector-based message routing, worker thread pools, and message bus integration (libatbus).

- **Repository**: https://github.com/atframework/libatapp
- **License**: MIT
- **Languages**: C++ (C++17 required, C++17/C++20/C++23 features used when available)

## Skills (How-to playbooks)

Operational, copy/paste-friendly guides live in `.agents/skills/`:

- Entry point: `.agents/skills/README.md`

## Build System

This project uses **CMake** (minimum version 3.24.0).

Build steps and common configuration options are documented in:

- `.agents/skills/build/SKILL.md`

## Directory Structure

```
libatapp/
├── include/               # Public headers
│   └── atframe/           # Main include directory
│       ├── atapp.h                 # Main application class
│       ├── atapp_conf.h            # Configuration structures (app_conf, bus_conf)
│       ├── atapp_conf.proto        # Protobuf v3 config definition (source of truth)
│       ├── atapp_common_types.h    # Common typedefs (app_id_t, jiffies_timer_t)
│       ├── atapp_module_impl.h     # Module interface (lifecycle hooks)
│       ├── atapp_log_sink_maker.h  # Log sink factory (file/stdout/stderr/syslog)
│       ├── connectors/             # Connector layer
│       │   ├── atapp_connector_impl.h     # Base connector (address_type_t, virtual interface)
│       │   ├── atapp_connector_atbus.h    # atbus connector (topology-aware routing)
│       │   ├── atapp_connector_loopback.h # Loopback connector (self-message)
│       │   ├── atapp_connection_handle.h  # Connection handle (ready/closing flags, priv data)
│       │   └── atapp_endpoint.h           # Endpoint (pending message queue, discovery binding)
│       ├── etcdcli/                # etcd client layer
│       │   ├── etcd_cluster.h             # etcd HTTP client (KV/watch/lease, stats)
│       │   ├── etcd_def.h                 # enums (etcd_discovery_action_t: kPut/kDelete)
│       │   ├── etcd_discovery_node.h      # Single discovered node (version, gateway rotation)
│       │   ├── etcd_discovery_set.h       # Node set (consistent_hash, round_robin, random)
│       │   ├── etcd_keepalive.h           # Lease-bound key keepalive
│       │   ├── etcd_watcher.h             # Key-range watcher with event callbacks
│       │   └── etcd_packer.h              # Serialization (RapidJSON, base64, key range)
│       └── modules/                # Built-in modules
│           ├── etcd_module.h              # etcd integration module (discovery/topology)
│           ├── worker_pool_module.h       # Worker thread pool (scaling, job spawn)
│           └── worker_context.h           # Worker thread context (worker_id)
├── src/                   # Implementation
│   ├── atframe/           # Core implementation
│   │   ├── atapp.cpp              # App lifecycle, message routing, timers (~4800 lines)
│   │   ├── atapp_conf.cpp         # Config loading (YAML/INI/env/JSON, expression expand)
│   │   ├── atapp_module_impl.cpp  # Module lifecycle, suspend_stop
│   │   ├── atapp_log_sink_maker.cpp   # Log sink creation
│   │   ├── connectors/
│   │   │   ├── atapp_connector_impl.cpp     # Connector base, protocol registry
│   │   │   ├── atapp_connector_atbus.cpp    # ATBus routing, reconnect timers
│   │   │   ├── atapp_connector_loopback.cpp # Loopback send/receive
│   │   │   ├── atapp_connection_handle.cpp  # Handle lifecycle, flags
│   │   │   └── atapp_endpoint.cpp           # Pending messages, discovery update, GC
│   │   ├── etcdcli/
│   │   │   ├── etcd_cluster.cpp    # HTTP request lifecycle, lease grant/renew
│   │   │   ├── etcd_discovery_node.cpp  # Node version compare, gateway selection
│   │   │   ├── etcd_discovery_set.cpp   # Consistent hash ring, metadata filter
│   │   │   ├── etcd_keepalive.cpp  # Periodic set + checker function
│   │   │   ├── etcd_watcher.cpp    # Long-poll watch, revision tracking
│   │   │   └── etcd_packer.cpp     # JSON⇄proto, base64, key range prefix
│   │   └── modules/
│   │       ├── etcd_module.cpp     # Discovery/topology watcher lifecycle
│   │       └── worker_pool_module.cpp   # Thread spawn, scaling, tick dispatch
│   └── log/               # Log initialization
├── test/                  # Unit tests
│   └── case/              # Test case files (63+ test cases across 12 files)
│       ├── atapp_setup_test.cpp               # 1 test: init timeout
│       ├── atapp_message_test.cpp             # 2 tests: remote + loopback
│       ├── atapp_connector_test.cpp           # 1 test: address type classify
│       ├── atapp_direct_connect_test.cpp      # 9 tests (B.1–B.9): direct peer topology
│       ├── atapp_upstream_forward_test.cpp    # 7 tests (A.1–A.7): upstream proxy
│       ├── atapp_discovery_test.cpp           # 12 tests: metadata, hash, round_robin
│       ├── atapp_etcd_cluster_test.cpp        # 7 tests: etcd client (req etcd)
│       ├── atapp_etcd_module_test.cpp         # 8 tests: etcd integration (req etcd)
│       ├── atapp_etcd_packer_test.cpp         # 7 tests: pack/unpack, base64
│       ├── atapp_configure_loader_test.cpp    # 4 tests: YAML/INI/env/expression
│       ├── atapp_worker_pool_test.cpp         # 5 tests: spawn, stop, foreach, tick
│       └── *.yaml                             # Test configuration files
├── sample/                # Sample applications
├── binding/               # Language bindings
└── tools/                 # Utility tools
```

## Architecture Layers

```
┌──────────────────────────────────────────────────────────────────────┐
│  Application (send_message / evt_on_forward_request callback)       │
├──────────────────────────────────────────────────────────────────────┤
│  Module System    - lifecycle hooks, suspend_stop, tick             │
│  ├─ etcd_module   - discovery/topology watchers, keepalive actors  │
│  └─ worker_pool   - thread pool, job spawn, scaling                │
├──────────────────────────────────────────────────────────────────────┤
│  Connector Layer  - address type, send/receive, protocol registry  │
│  ├─ connector_atbus    - topology-aware routing (direct/proxy)     │
│  ├─ connector_loopback - self-message                              │
│  └─ connection_handle  - ready/closing state, private data         │
├──────────────────────────────────────────────────────────────────────┤
│  Endpoint         - remote node, pending message queue, GC         │
├──────────────────────────────────────────────────────────────────────┤
│  etcd Client      - cluster, lease, watcher, keepalive             │
├──────────────────────────────────────────────────────────────────────┤
│  Discovery        - discovery_node, discovery_set (hash/robin/rng) │
├──────────────────────────────────────────────────────────────────────┤
│  Custom Timers    - jiffies_timer<8,3,9>, system_clock-based       │
├──────────────────────────────────────────────────────────────────────┤
│  Configuration    - YAML/INI/env/JSON, protobuf, expression expand │
├──────────────────────────────────────────────────────────────────────┤
│  libatbus         - underlying message bus transport                │
├──────────────────────────────────────────────────────────────────────┤
│  libuv            - event loop                                     │
└──────────────────────────────────────────────────────────────────────┘
```

## Application Lifecycle

```
new app()
  │
  ▼
init(ev_loop, argc, argv)
  ├── Parse CLI (--conf, -id, -name, etc.)
  ├── Load config (YAML/INI/env/JSON) → expression expansion
  ├── Setup log sinks
  ├── module_impl::setup(conf) for each module
  ├── Init atbus node (listen, connect)
  ├── module_impl::init() for each module
  ├── module_impl::ready() for each module
  └── evt_on_all_module_inited callback
  │
  ▼
run() / run_noblock() / tick()
  ├── Process signals (SIGINT → stop)
  ├── uv_run() event loop (run/run_noblock)
  ├── Process incoming messages → evt_on_forward_request
  ├── module_impl::tick() for each module
  ├── process_custom_timers() → jiffies timer callbacks
  ├── Endpoint GC (pending message timeout)
  └── atbus tick (keepalive, reconnect)
  │
  ▼
stop()
  ├── module_impl::stop() for each module (may suspend_stop)
  ├── Wait for async cleanup or timeout
  ├── module_impl::timeout() if deadline exceeded
  ├── module_impl::cleanup() for each module
  └── evt_on_all_module_cleaned callback
  │
  ▼
~app()
  └── add_evt_on_finally callbacks fired
```

### Differences Between run(), run_noblock(), and tick()

- `run()` — blocking; calls `uv_run(UV_RUN_DEFAULT)`, returns when app stops
- `run_noblock()` — non-blocking; calls `uv_run(UV_RUN_NOWAIT)` once, processes I/O + timers
- `tick()` — no I/O; processes only app-level timers and module ticks (no libuv event loop)

### Application State Flags

```cpp
flag_t:
  kRunning = 0         // App is running
  kStoping = 1         // Stop requested
  kTimeout = 2         // Stop timeout exceeded
  kInCallback = 3      // Inside event callback
  kInitialized = 4     // Init completed
  kInitializing = 5    // Init in progress
  kStopped = 6         // Fully stopped
  kDisableAtbusFallback = 7  // Disable atbus as default connector
  kInTick = 8          // Inside tick()
  kDestroying = 9      // Destructor running
```

## Module System

Modules implement `atframework::atapp::module_impl` and provide lifecycle hooks.

For detailed architecture, lifecycle diagrams, connector integration, and code examples, see `.agents/skills/libatapp-module-connector/SKILL.md`.

### Module Lifecycle Hooks (call order)

| Hook          | Return | When Called                | Notes                                          |
| ------------- | ------ | -------------------------- | ---------------------------------------------- |
| `on_bind()`   | void   | Module added to app        | Access `get_app()` after this                  |
| `setup(conf)` | int    | Before init, config loaded | Inspect/modify config                          |
| `init()`      | int    | After setup, once only     | **Pure virtual** — must implement              |
| `ready()`     | void   | After all modules init     | App is fully ready                             |
| `prereload()` | void   | Before config reload       | Pre-reload hook                                |
| `reload()`    | int    | Config reloaded            | Re-read config values                          |
| `setup_log()` | int    | Log configuration changed  | Reconfigure log sinks                          |
| `tick()`      | int    | Every tick interval        | Return >0 = busy (prevents sleep)              |
| `stop()`      | int    | App stopping               | Return non-zero = async (will be called again) |
| `timeout()`   | int    | Stop deadline exceeded     | Force-stop; called after stop timeout          |
| `cleanup()`   | void   | After stop/timeout         | Final cleanup                                  |
| `on_unbind()` | void   | Module removed from app    | Last hook                                      |

### suspend_stop

Modules can delay shutdown for async cleanup:

```cpp
int my_module::stop() override {
    suspend_stop(std::chrono::seconds(30), [this]() -> bool {
        return all_connections_closed();  // return true when done
    });
    return 1;  // non-zero = still stopping
}
```

### Built-in Modules

- **etcd_module** (`modules/etcd_module.h`) — Service discovery/topology via etcd; manages keepalive actors, discovery watchers, topology registration
- **worker_pool_module** (`modules/worker_pool_module.h`) — Worker thread pool with scaling modes (kStable, kDynamic, kPendingToDestroy), CPU time stats, job spawn, tick callbacks

## Connector System

Connectors provide the transport layer for inter-node communication.

For detailed connector architecture and endpoint management, see `.agents/skills/libatapp-module-connector/SKILL.md`.

### Connector Types

| Connector                  | Description                                          | Address Types                      |
| -------------------------- | ---------------------------------------------------- | ---------------------------------- |
| `atapp_connector_atbus`    | libatbus message bus with topology-aware routing     | kDuplex, kLocalHost, kLocalProcess |
| `atapp_connector_loopback` | Self-message delivery (app sends to itself)          | kLocalProcess                      |
| Custom connectors          | Inherit `atapp_connector_impl` for custom transports | Any combination                    |

### Address Type Classification

```cpp
address_type_t (bitfield):
  kNone         = 0x0000  // Unknown
  kDuplex       = 0x0001  // Full duplex (TCP, etc.)
  kSimplex      = 0x0002  // One-way only
  kLocalHost    = 0x0004  // Same host (shm://, unix://)
  kLocalProcess = 0x0008  // Same process (mem://)
```

### ATBus Connector Routing

The `atapp_connector_atbus` connector performs topology-aware routing:

1. **Direct**: Target is a known peer → send via direct atbus connection
2. **Via Upstream**: Target is in a different subtree → forward to upstream proxy
3. **Via Downstream**: Target is a child → route down the tree

Reconnect logic uses exponential backoff with configurable `reconnect_tick_interval` and `reconnect_max_try_times`.

### Connection Handle

`atapp_connection_handle` wraps a connection with:

- `kReady` / `kClosing` flags
- Private data pointer (`get_private_data<T>()` / `set_private_data()`)
- `on_destroy` callback for cleanup

### Endpoint

`atapp_endpoint` represents a remote node:

- Binds to an `etcd_discovery_node` for metadata
- Maintains a **pending message queue** (when no ready connection exists)
- Messages are retried when connection becomes ready (`retry_pending_messages()`)
- GC timeout removes stale endpoints

## Message Routing

### Sending Messages

```cpp
// By node ID
app.send_message(target_node_id, type, data_span);

// By node name
app.send_message("service_name", type, data_span);

// By discovery node
app.send_message(discovery_node_ptr, type, data_span);

// Load-balancing across a discovery set
app.send_message_by_consistent_hash(hash_key, type, data_span);
app.send_message_by_round_robin(type, data_span);
app.send_message_by_random(type, data_span);

// With custom discovery set
app.send_message_by_consistent_hash(discovery_set, hash_key, type, data_span);
```

### Message Flow

```
send_message(target_id, type, data)
  ├── Resolve target → find/create endpoint
  ├── If endpoint has ready connection:
  │   └── connector.on_send_forward_request() → atbus send_data
  └── Else:
      └── endpoint.push_forward_message() → pending queue
          └── When connection ready: retry_pending_messages()

Receive:
  atbus on_forward_request callback
    └── app.evt_on_forward_request(app, message_sender_t, message_t)
```

### message_sender_t

```cpp
struct message_sender_t {
    app_id_t id;            // Source node ID
    std::string name;       // Source node name
    const void *remote;     // Pointer to endpoint (if available)
};
```

## Event Callbacks

```cpp
// Message events
app.set_evt_on_forward_request(fn)    // Receive incoming message
app.set_evt_on_forward_response(fn)   // Send completion notification

// Connection events
app.set_evt_on_app_connected(fn)      // Remote node connected
app.set_evt_on_app_disconnected(fn)   // Remote node disconnected

// Lifecycle events
app.set_evt_on_all_module_inited(fn)  // All modules initialized
app.set_evt_on_all_module_cleaned(fn) // All modules cleaned up

// Finally callbacks (after destructor)
app.add_evt_on_finally(fn)            // Returns handle for removal
app.remove_evt_on_finally(handle)
app.clear_evt_on_finally()
```

### Callback Signatures

```cpp
callback_fn_on_forward_request_t   = std::function<int(app &, const message_sender_t &, const message_t &)>
callback_fn_on_forward_response_t  = std::function<int(app &, const message_sender_t &, const message_t &, int32_t)>
callback_fn_on_connected_t         = std::function<int(app &, atbus::endpoint &, int)>
callback_fn_on_disconnected_t      = std::function<int(app &, atbus::endpoint &, int)>
callback_fn_on_all_module_inited_t = std::function<int(app &)>
callback_fn_on_all_module_cleaned_t= std::function<int(app &)>
callback_fn_on_finally_t           = std::function<void(app &)>
```

## Custom Timer System

libatapp uses `jiffies_timer<8, 3, 9>` — a hashed hierarchical timing wheel.

```cpp
// Add a timer with delta duration
app.add_custom_timer(std::chrono::seconds(5), [](jiffies_timer_t &, const jiffies_timer_t::timer_t &timer) {
    // Timer fired
}, priv_data, &watcher);

// Add a timer with absolute time point
app.add_custom_timer_with_system_clock(deadline_tp, callback, priv_data, &watcher);

// Cancel a timer
app.remove_custom_timer(watcher);
```

**Important**: The jiffies timer controller is lazily initialized. If `add_custom_timer` is called before the first `tick()`, it may fail. The timer is initialized during `process_custom_timers()` which runs inside `tick()`.

## etcd Integration

For detailed etcd client API, discovery system, and topology management, see `.agents/skills/libatapp-etcd-discovery/SKILL.md`.

### etcd Client (`etcd_cluster`)

HTTP-based etcd v3 client with:

- **KV operations**: `create_request_kv_get()`, `create_request_kv_set()`, `create_request_kv_del()`
- **Watch**: `create_request_watch()` for key range changes
- **Lease**: Automatic grant/renew, `get_lease()`
- **Stats**: `sum_error_requests`, `continue_error_requests`, `sum_success_requests`, `sum_create_requests`
- **Events**: `add_on_event_up()` / `add_on_event_down()` for cluster availability changes
- **TLS**: Optional SSL/TLS configuration

### Discovery System (`etcd_discovery_set`)

Node selection algorithms:

| Method                             | Description                                       |
| ---------------------------------- | ------------------------------------------------- |
| `get_node_by_id(id)`               | Lookup by node ID                                 |
| `get_node_by_name(name)`           | Lookup by node name                               |
| `get_node_by_consistent_hash(key)` | Consistent hash ring (uint64, int64, string, buf) |
| `get_node_by_round_robin()`        | Round-robin rotation                              |
| `get_node_by_random()`             | Random selection                                  |

All selection methods accept optional `metadata_type *metadata` for filtering. The `metadata_equal_type::filter()` checks metadata field matching.

### etcd Module (`etcd_module`)

Integrates etcd with the app lifecycle:

- **Discovery**: Watches for service node registrations, maintains `global_discovery` set
- **Topology**: Watches for topology info, maintains `topology_info_set`
- **Keepalive**: Registers the app's own discovery info with etcd lease binding
- **Snapshots**: Loading and loaded snapshot event callbacks for initial state sync

Key APIs:

```cpp
etcd_module->get_global_discovery()               // Access the discovery set
etcd_module->add_on_node_discovery_event(fn)      // Node put/delete events
etcd_module->add_on_topology_info_event(fn)       // Topology change events
etcd_module->add_keepalive_actor(val, path)       // Register custom keepalive
etcd_module->get_raw_etcd_ctx()                   // Raw etcd_cluster for custom ops
```

## Configuration

Configuration is defined in `atapp_conf.proto` and can be loaded from:

- **YAML** files (`.yaml`) — preferred format
- **INI** files (`.conf`)
- **Environment variables**
- **JSON** (via RapidJSON)
- **Command-line arguments** (`--conf`, `-id`, `-name`, etc.)

Example configuration file (`app.yaml`):

```yaml
id: 0x12345678
name: "my_service"
bus:
  listen:
    - "ipv4://0.0.0.0:12345"
  first_idle_timeout: 3600
  ping_interval: 60
timer:
  tick_interval: 32ms
log:
  level: info
etcd:
  hosts:
    - "http://127.0.0.1:2379"
  path: "/atapp/services/my_cluster"
```

### Expression Expansion in Configuration

Protobuf fields annotated with `enable_expression: true` in the `atapp_configure_meta` extension
support **environment-variable expression expansion** at config-load time.

See `.agents/skills/configure-expression/SKILL.md` for the full syntax reference and how-to guide.

## Worker Pool

The `worker_pool_module` provides multi-threaded job execution:

- **Scaling modes**: kStable (fixed workers), kDynamic (auto-scaled), kPendingToDestroy (winding down)
- **Job spawn**: Submit jobs to worker threads
- **Tick callbacks**: Per-tick callbacks on worker threads
- **CPU statistics**: Track CPU time per worker
- **foreach_stable_workers**: Iterate over stable workers

## Unit Testing Framework

This project uses the **same private unit testing framework** as atframe_utils and libatbus (not Google Test).

### Test Framework Macros

```cpp
CASE_TEST(test_group_name, test_case_name) {
    // Test implementation
}

// Assertions
CASE_EXPECT_TRUE(condition)
CASE_EXPECT_FALSE(condition)
CASE_EXPECT_EQ(expected, actual)
CASE_EXPECT_NE(val1, val2)
CASE_EXPECT_LT(val1, val2)
CASE_EXPECT_LE(val1, val2)
CASE_EXPECT_GT(val1, val2)
CASE_EXPECT_GE(val1, val2)
CASE_EXPECT_ERROR(message)

// Logging during tests
CASE_MSG_INFO() << "Info message";
CASE_MSG_ERROR() << "Error message";

// Test utilities
CASE_THREAD_SLEEP_MS(milliseconds)
CASE_THREAD_YIELD()
```

### Test Groups

| Group                    | File                              | Tests | Description                                  |
| ------------------------ | --------------------------------- | ----- | -------------------------------------------- |
| `atapp_setup`            | `atapp_setup_test.cpp`            | 1     | Initialization timeout handling              |
| `atapp_message`          | `atapp_message_test.cpp`          | 2     | Remote send + loopback                       |
| `atapp_connector`        | `atapp_connector_test.cpp`        | 1     | Address type classification                  |
| `atapp_direct_connect`   | `atapp_direct_connect_test.cpp`   | 9     | Direct peer topology (3 nodes, B.1–B.9)      |
| `atapp_upstream`         | `atapp_upstream_forward_test.cpp` | 7     | Upstream proxy forwarding (3 nodes, A.1–A.7) |
| `atapp_discovery`        | `atapp_discovery_test.cpp`        | 12    | Metadata filter, hash, round_robin, stress   |
| `atapp_etcd_cluster`     | `atapp_etcd_cluster_test.cpp`     | 7     | etcd client ops (requires etcd)              |
| `atapp_etcd_module`      | `atapp_etcd_module_test.cpp`      | 8     | etcd module integration (requires etcd)      |
| `atapp_etcd_packer`      | `atapp_etcd_packer_test.cpp`      | 7     | KV pack/unpack, base64, key range            |
| `atapp_configure_loader` | `atapp_configure_loader_test.cpp` | 4     | YAML/INI/env load, expression expansion      |
| `atapp_worker_pool`      | `atapp_worker_pool_test.cpp`      | 5     | Spawn, stop, foreach, tick                   |

### Multi-Node Test Patterns

The direct-connect and upstream-forward tests use helper structs for multi-app testing:

```cpp
// Common pattern: 3-node topology
struct three_node_apps {
    app node1, upstream, node3;
    void full_setup_and_connect();  // Init all, pump until connected
};

// Event loop helper
void pump_until(std::function<bool()> condition, std::chrono::milliseconds timeout);

// Debug-only time control (for timer/timeout tests)
app::set_sys_now(time_point);  // Jump virtual time forward
app.tick();                     // Process timers without I/O
```

**Important**: When multiple apps share `uv_default_loop()` (initialized with `init(nullptr, ...)`), calling `run_noblock()` fires libuv timers for ALL apps. Use `tick()` instead of `run_noblock()` during virtual-time-advance sections to avoid cross-app interference.

### Local etcd for Testing

The `ci/etcd/` directory provides scripts to **download, start, stop, and clean up** a local etcd instance for running etcd-dependent unit tests.

**Windows (PowerShell):**

```powershell
# Download etcd binary (auto-detects latest version)
.\ci\etcd\setup-etcd.ps1 -Command download

# Start etcd on default ports (client: 12379, peer: 12380)
.\ci\etcd\setup-etcd.ps1 -Command start

# Set env var so unit tests discover the etcd instance
$env:ATAPP_UNIT_TEST_ETCD_HOST = "http://127.0.0.1:12379"

# Run etcd-dependent tests
./atapp_unit_test.exe -r atapp_etcd_cluster
./atapp_unit_test.exe -r atapp_etcd_module

# Check status / stop / cleanup
.\ci\etcd\setup-etcd.ps1 -Command status
.\ci\etcd\setup-etcd.ps1 -Command stop
.\ci\etcd\setup-etcd.ps1 -Command cleanup   # stop + delete binaries and data
```

**Linux / macOS (Bash):**

```bash
# Download etcd binary
bash ci/etcd/setup-etcd.sh download

# Start etcd
bash ci/etcd/setup-etcd.sh start

# Set env var
export ATAPP_UNIT_TEST_ETCD_HOST="http://127.0.0.1:12379"

# Run tests
./atapp_unit_test -r atapp_etcd_cluster
./atapp_unit_test -r atapp_etcd_module

# Stop / cleanup
bash ci/etcd/setup-etcd.sh stop
bash ci/etcd/setup-etcd.sh cleanup
```

**Options:**

| Option (PS1)   | Option (sh)      | Default                | Description                      |
| -------------- | ---------------- | ---------------------- | -------------------------------- |
| `-WorkDir`     | `--work-dir`     | `$TEMP/etcd-unit-test` | Working directory for binaries   |
| `-ClientPort`  | `--client-port`  | `12379`                | etcd client listen port          |
| `-PeerPort`    | `--peer-port`    | `12380`                | etcd peer listen port            |
| `-EtcdVersion` | `--etcd-version` | `latest`               | etcd version tag (e.g. `v3.5.9`) |

### Running and writing tests

See `.agents/skills/testing/SKILL.md`.

## Logging

Use the FWLOG macros for logging:

```cpp
FWLOGTRACE("Trace message: {}", value);
FWLOGDEBUG("Debug message: {}", value);
FWLOGINFO("Info message: {}", value);
FWLOGWARNING("Warning message: {}", value);
FWLOGERROR("Error message: {}", value);
FWLOGFATAL("Fatal message: {}", value);
```

Log sink types: file (with rotation), stdout, stderr, syslog. Created via `log_sink_maker`.

## Code Formatting

This project uses **clang-format** for code formatting. The `.clang-format` file is located at the project root.

- Style: Based on Google style
- Column limit: 120
- Run formatting: `clang-format -i <file>`

## Coding Conventions

1. **Namespaces**: `atframework::atapp` for the library, `atframework::atapp::protocol` for protobuf types
2. **Include guards**: Use `#pragma once`
3. **C++ Standard**: C++17 required
4. **Naming**:
   - Classes/structs: `snake_case`
   - Functions: `snake_case`
   - Constants: `UPPER_SNAKE_CASE`
   - Types: `*_t` suffix for typedefs
5. **Smart pointers**: Use `std::shared_ptr` for modules and app components
6. **Anonymous namespace + static**: In `.cpp` files, file-local functions should be placed inside an anonymous namespace **and** keep the `static` keyword. Do **not** remove `static` when moving a function into an anonymous namespace.
   ```cpp
   namespace {
   static void my_helper() { /* ... */ }
   }  // namespace
   ```
7. **Error handling**: Return `int` / `int32_t` error codes (0 = success, negative = error)
8. **Arena allocation**: Protobuf messages use `google::protobuf::Arena` for efficient allocation

## Compiler Support

| Compiler | Minimum Version |
| -------- | --------------- |
| GCC      | 7.1+            |
| Clang    | 7+              |
| MSVC     | VS2022+         |

## Dependencies

### Required

- **atframe_utils** — Core utility library (jiffies_timer, time_utility, etc.)
- **libatbus** — Message bus transport
- **libuv** — Async I/O event loop
- **protobuf** — Configuration serialization (`atapp_conf.proto`)

### Optional

- **yaml-cpp** — YAML configuration support
- **RapidJSON** — JSON configuration / etcd response parsing
- **libcurl** — etcd HTTP client
- **OpenSSL / MbedTLS** — TLS for etcd connections

## Key Components Quick Reference

### Application (`atapp.h`)

```cpp
#include <atframe/atapp.h>

int main(int argc, char *argv[]) {
    atframework::atapp::app app;

    // Setup event handlers
    app.set_evt_on_forward_request(my_message_handler);
    app.set_evt_on_forward_response(my_response_handler);

    // Add modules
    app.add_module(std::make_shared<my_module>());

    // Run (blocking)
    return app.run(uv_default_loop(), argc, (const char**)argv, nullptr);
}
```

### Custom Modules

```cpp
#include <atframe/atapp_module_impl.h>

class my_module : public atframework::atapp::module_impl {
public:
    virtual int init() override {
        FWLOGINFO("my_module init");
        return 0;
    }

    virtual int reload() override { return 0; }

    virtual int stop() override { return 0; }

    virtual int timeout() override { return 0; }

    virtual const char* name() const override { return "my_module"; }

    virtual int tick() override { return 0; }
};
```

### Custom Command-Line Options

```cpp
auto opt_mgr = app.get_option_manager();
opt_mgr->bind_cmd("-echo", app_option_handler_echo)
    ->set_help_msg("-echo [text]    echo a message.");
```

### Custom Remote Commands

```cpp
auto cmgr = app.get_command_manager();
cmgr->bind_cmd("echo", app_command_handler_echo)
    ->set_help_msg("echo [messages...]    echo messages to log");
// Usage: ./myapp -id ID --conf CONFIG_FILE run echo MESSAGE...
```

## Sample Applications

See the `sample/` directory for complete examples:

- Minimal server
- Echo server with custom commands
- Multi-module application
