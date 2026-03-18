---
name: libatapp-module-connector
description: libatapp module system, connector architecture, endpoint lifecycle, connection handles, and message routing patterns. Use when writing modules, connectors, endpoint management, or multi-node tests.
---

# Module System & Connector Architecture (libatapp)

## Module System

### Module Lifecycle

Modules inherit `atframework::atapp::module_impl` and are registered with `app.add_module(ptr)`.

**Lifecycle call order:**

```
on_bind()        ← Module added to app (get_app() available after)
  │
setup(conf)      ← Config loaded, before init (can modify conf)
  │
init()           ← Pure virtual; must implement (return 0 = success)
  │
ready()          ← All modules initialized; app fully ready
  │
┌─── tick loop ───────────────────────────────────────────┐
│ tick()         ← Called every timer.tick_interval        │
│                  Return > 0 = busy (prevents sleep)     │
│ prereload()    ← Before config reload (if triggered)    │
│ reload()       ← After config reloaded                  │
│ setup_log()    ← Log config changed                     │
└─────────────────────────────────────────────────────────┘
  │
stop()           ← App stopping; return non-zero = async stop
  │
timeout()        ← Called if stop exceeds deadline
  │
cleanup()        ← Final resource release
  │
on_unbind()      ← Module removed from app
```

### Writing a Module

```cpp
#include <atframe/atapp_module_impl.h>

class my_module : public atframework::atapp::module_impl {
public:
    int init() override {
        // Access app config: get_app()->get_origin_configure()
        // Access bus node: get_app()->get_bus_node()
        return 0;  // 0 = success
    }

    int reload() override {
        // Re-read configuration values
        return 0;
    }

    int stop() override {
        if (!all_done()) {
            // Async stop: will be called again
            suspend_stop(std::chrono::seconds(30), [this]() {
                return all_done();
            });
            return 1;  // non-zero = still stopping
        }
        return 0;  // 0 = stop complete
    }

    int timeout() override {
        // Force-stop: deadline exceeded
        force_close_everything();
        return 0;
    }

    const char* name() const override { return "my_module"; }

    int tick() override {
        // Periodic work
        int busy = process_pending();
        return busy;  // > 0 = busy
    }
};
```

### suspend_stop

Delays module shutdown until a condition is met or timeout expires:

```cpp
suspend_stop(std::chrono::seconds(30), []() -> bool {
    return condition_met;  // return true = done, resume stop
});
```

- The check function is called each tick during stop phase
- If timeout expires without returning true, `timeout()` is called
- Multiple modules can suspend_stop concurrently; app waits for all

### Module Access Patterns

```cpp
// Inside a module:
get_app()                             // → app& (available after on_bind)
get_app()->get_id()                   // → app_id_t (bus ID)
get_app()->get_app_name()             // → const std::string&
get_app()->get_origin_configure()     // → const atapp::protocol::atapp_configure&
get_app()->get_bus_node()             // → atbus::node*
get_app()->is_running()               // → bool
get_app()->is_closing()               // → bool
get_app()->get_last_tick_time()       // → time_point
```

## Connector Architecture

### Connector Hierarchy

```
atapp_connector_impl (base)
  ├── atapp_connector_atbus    (libatbus message bus)
  └── atapp_connector_loopback (self-message delivery)
```

### Connector Interface

```cpp
class atapp_connector_impl {
public:
    // Address classification (pure virtual)
    virtual uint32_t get_address_type(const channel_address_t &addr) const noexcept = 0;

    // Lifecycle hooks
    virtual int32_t on_start_listen(const channel_address_t &addr);
    virtual int32_t on_start_connect(
        const etcd_discovery_node &discovery,
        atapp_endpoint &endpoint,
        const channel_address_t &addr,
        const atapp_connection_handle::ptr_t &handle);
    virtual int32_t on_close_connection(atapp_connection_handle &handle);

    // Message I/O
    virtual int32_t on_send_forward_request(
        atapp_connection_handle *handle,
        int32_t type,
        uint64_t *msg_sequence,
        gsl::span<const unsigned char> data,
        const atapp::protocol::atapp_metadata *metadata);
    virtual void on_receive_forward_response(...);

    // Discovery events
    virtual void on_discovery_event(etcd_discovery_action_t, const etcd_discovery_node::ptr_t &);
};
```

### Address Types (bitfield)

```cpp
address_type_t:
  kNone         = 0x0000   // Unknown / not handled
  kDuplex       = 0x0001   // Full duplex (e.g., TCP)
  kSimplex      = 0x0002   // One-way only
  kLocalHost    = 0x0004   // Same machine (e.g., shm://, unix://)
  kLocalProcess = 0x0008   // Same process (e.g., mem://)
```

Address scheme to type mapping (atbus connector):

| Scheme                         | Address Type             |
| ------------------------------ | ------------------------ |
| `mem://`                       | kDuplex \| kLocalProcess |
| `shm://`                       | kDuplex \| kLocalHost    |
| `unix://`                      | kDuplex \| kLocalHost    |
| `ipv4://`, `ipv6://`, `dns://` | kDuplex                  |
| `atcp://`                      | kDuplex                  |

### Writing a Custom Connector

```cpp
class my_connector : public atframework::atapp::atapp_connector_impl {
public:
    uint32_t get_address_type(const channel_address_t &addr) const noexcept override {
        if (addr.scheme == "myproto") {
            return address_type_t::kDuplex;
        }
        return address_type_t::kNone;  // Don't handle this scheme
    }

    int32_t on_start_connect(
        const etcd_discovery_node &discovery,
        atapp_endpoint &endpoint,
        const channel_address_t &addr,
        const atapp_connection_handle::ptr_t &handle) override {
        // Establish connection to remote node
        // When ready: handle->set_ready()
        return 0;
    }

    int32_t on_send_forward_request(
        atapp_connection_handle *handle,
        int32_t type,
        uint64_t *msg_sequence,
        gsl::span<const unsigned char> data,
        const atapp::protocol::atapp_metadata *metadata) override {
        // Send data via custom transport
        return 0;
    }
};
```

## ATBus Connector Routing

The `atapp_connector_atbus` performs topology-aware routing in `try_connect_to()`:

```
Target resolution:
  1. Is target self?         → loopback connector
  2. Is target a known peer? → direct atbus connection
  3. Same upstream parent?   → direct connection attempt
  4. Target is upstream?     → connect directly to upstream
  5. Target is downstream?   → route down the tree
  6. Otherwise:              → forward to upstream proxy
```

### Reconnect Logic

- Reconnect uses exponential backoff
- Configurable: `reconnect_tick_interval` (base interval), `reconnect_max_try_times` (max retries)
- Each failed attempt increases the backoff multiplier
- Timer is managed via `jiffies_timer_watcher_t` (can cancel with `remove_custom_timer`)

### Handle Map

The atbus connector maintains a handle map keyed by `bus_id`:

```cpp
// Internal: bus_id → connection_handle mapping
// Used for fast lookup when routing messages
// Handles are added on connection, removed on disconnect
```

## Connection Handle

`atapp_connection_handle` wraps an individual connection:

```cpp
struct flags_t {
    kReady   = 0,   // Connection is ready for I/O
    kClosing = 1,   // Connection is shutting down
};

// Key methods:
handle->check_flag(flags_t::kReady)    // Is connection ready?
handle->set_flag(flags_t::kReady, true)
handle->get_private_data<MyType>()     // Typed private data
handle->set_private_data(ptr, destructor_fn)
handle->on_destroy = callback          // Cleanup callback
```

## Endpoint Lifecycle

`atapp_endpoint` represents a remote node with message queuing:

```
Discovery event (node discovered)
  │
  ▼
Create endpoint → bind discovery_node
  │
  ▼
Connector attempts connection → creates connection_handle
  │
  ▼
Connection ready (handle.kReady = true)
  ├── Retry pending messages → send via connector
  └── New messages sent directly
  │
  ▼
Connection lost
  ├── New messages queued as pending
  └── Connector reconnects (if applicable)
  │
  ▼
GC timeout → endpoint removed (if no activity)
```

### Pending Message Queue

When no ready connection exists, messages are queued:

```cpp
struct pending_message_t {
    raw_time_t expired_timepoint;  // When message expires
    int32_t type;                  // Message type
    uint64_t message_sequence;     // Unique sequence ID
    std::vector<unsigned char> data;
    std::unique_ptr<atapp::protocol::atapp_metadata> metadata;
};
```

- `push_forward_message()` — queue a message
- `retry_pending_messages()` — send queued messages when connection becomes ready
- `get_pending_message_count()` / `get_pending_message_size()` — inspect queue
- Messages expire based on `expired_timepoint`; expired messages are discarded

## Multi-Node Test Patterns

### Three-Node Setup (Direct Connect)

```cpp
// From atapp_direct_connect_test.cpp
struct direct_three_node_apps {
    app node1;      // 0x201
    app node2;      // 0x202
    app upstream;   // 0x203

    void full_setup_and_connect() {
        // 1. Init upstream first
        // 2. Init node1 and node2
        // 3. Inject discovery nodes
        // 4. Pump until all connected
    }
};
```

### Three-Node Setup (Upstream Proxy)

```cpp
// From atapp_upstream_forward_test.cpp
struct three_node_apps {
    app node1;      // 0x101 (allow_direct_connection: false)
    app upstream;   // 0x102
    app node3;      // 0x103

    // node1 and node3 communicate via upstream proxy
};
```

### Event Loop Helpers

```cpp
// Pump event loop until condition or timeout
void pump_until(std::function<bool()> condition, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        for (auto &a : apps) {
            a.run_noblock();
        }
        CASE_THREAD_SLEEP_MS(8);
    }
}

// Debug-only: virtual time control for timer tests
app::set_sys_now(future_time_point);  // Jump clock forward
app.tick();                            // Process timers (no I/O)
```

### Testing Caveats

1. **Shared uv_default_loop()**: When multiple apps use `init(nullptr, ...)`, they share the default libuv loop. `run_noblock()` fires timers for ALL apps. Use `tick()` in time-advance sections.

2. **set_sys_now() is static**: Affects ALL app instances. Set large bus timeouts (e.g., `first_idle_timeout: 3600`) in test configs to prevent atbus idle disconnect.

3. **Jiffies timer + set_sys_now()**: Only one timer callback fires per `tick()` call when time is jumped. Use multiple `set_sys_now()` + `tick()` rounds for multi-timer scenarios.

4. **Discovery injection**: Mock discovery by injecting nodes directly:
   ```cpp
   auto node = std::make_shared<etcd_discovery_node>();
   node->copy_from(discovery_info);
   etcd_module->get_global_discovery().add_node(node);
   ```

## Common Pitfalls

1. **Module init order**: `init()` is called in registration order. If module B depends on module A, register A first.

2. **Pending message overflow**: If connection never becomes ready and messages have long expiry, memory grows. Monitor `get_pending_message_size()`.

3. **Connector address overlap**: If two connectors handle the same scheme, the first registered one wins. Check `get_address_type()` returns `kNone` for schemes you don't handle.

4. **stop() return value**: Return 0 = stop complete. Return non-zero = will be called again. Forgetting to eventually return 0 causes the app to hang until timeout.

5. **Timer lazy init**: `add_custom_timer()` before the first `tick()` may fail because the jiffies timer controller isn't initialized yet. Call is safe after first `tick()` or after `process_custom_timers()` runs.
