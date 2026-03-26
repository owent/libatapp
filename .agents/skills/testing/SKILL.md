---
name: testing
description: Run and write unit tests for libatapp using the private test framework, including Windows DLL/PATH setup, test groups, multi-node test patterns, and debug-only time control.
---

# Unit testing (libatapp)

This repo uses a **private unit testing framework** (not GoogleTest).

## Run tests

The test executable is `atapp_unit_test`.

Common commands:

- Run all tests: `./atapp_unit_test`
- List tests: `./atapp_unit_test -l` / `./atapp_unit_test --list-tests`
- Run a group/case: `./atapp_unit_test -r <group>` or `./atapp_unit_test -r <group>.<case>`
- Filter: `./atapp_unit_test -f "pattern*"` / `./atapp_unit_test --filter "pattern*"`
- Help/version: `./atapp_unit_test -h`, `./atapp_unit_test -v`

## Windows: DLL lookup via PATH

On Windows, executables built in this repo commonly depend on DLLs placed under the build output tree (for example `publish/bin/<Config>`). If you run a unit test or sample directly from the build folder and see the process fail to start, Windows likely cannot find dependent DLLs.

Preferred approach: **prepend DLL directories to `PATH`** for the current run/debug session.

Typical DLL directories in the monorepo/toolset layout:

- `<BUILD_DIR>\\publish\\bin\\<Config>`
- `<REPO_ROOT>\\third_party\\install\\windows-amd64-msvc-19\\bin`

Example (PowerShell):

- `$buildDir = "<BUILD_DIR>"`
- `$cfg = "Debug"`
- `$env:PATH = "$buildDir\\publish\\bin\\$cfg;$buildDir\\publish\\bin;${PWD}\\third_party\\install\\windows-amd64-msvc-19\\bin;" + $env:PATH`
- `Set-Location "$buildDir\\_deps\\atapp\\test\\$cfg"`
- `./atapp_unit_test.exe -l`

## Test groups

| Group                       | File                                 | Tests | Requirements  | Description                             |
| --------------------------- | ------------------------------------ | ----- | ------------- | --------------------------------------- |
| `atapp_setup`               | `atapp_setup_test.cpp`               | 1     | —             | Init timeout handling                   |
| `atapp_message`             | `atapp_message_test.cpp`             | 2     | —             | Remote send + loopback                  |
| `atapp_connector`           | `atapp_connector_test.cpp`           | 1     | —             | Address type classification             |
| `atapp_upstream_forward`    | `atapp_upstream_forward_test.cpp`    | 8     | Debug build\* | Upstream proxy forwarding (A.1–A.8)     |
| `atapp_direct_connect`      | `atapp_direct_connect_test.cpp`      | 8     | Debug build\* | Direct peer topology (B.1–B.8)          |
| `atapp_downstream_send`     | `atapp_downstream_send_test.cpp`     | 4     | Debug build\* | Downstream send/pending (C.1–C.4)       |
| `atapp_topology_change`     | `atapp_topology_change_test.cpp`     | 9     | Debug build\* | Topology change/recovery (D.1–D.9)      |
| `atapp_discovery_reconnect` | `atapp_discovery_reconnect_test.cpp` | 5     | Debug build\* | Discovery reconnect logic (E.1–E.5)     |
| `atapp_error_recovery`      | `atapp_error_recovery_test.cpp`      | 5     | Debug build\* | Error recovery + cascade (F.1–F.5)      |
| `atapp_discovery`           | `atapp_discovery_test.cpp`           | 12    | —             | Metadata, hash, round_robin, stress     |
| `atapp_etcd_cluster`        | `atapp_etcd_cluster_test.cpp`        | 18    | etcd running  | etcd client operations                  |
| `atapp_etcd_module`         | `atapp_etcd_module_test.cpp`         | 20    | etcd running  | etcd module integration                 |
| `atapp_etcd_packer`         | `atapp_etcd_packer_test.cpp`         | 7     | —             | KV pack/unpack, base64, key range       |
| `atapp_configure`           | `atapp_configure_loader_test.cpp`    | 6     | —             | YAML/INI/env load, expression expansion |
| `atapp_worker_pool`         | `atapp_worker_pool_test.cpp`         | 5     | —             | Spawn, stop, foreach, tick              |

\* Many multi-node tests (A–F groups) use `set_sys_now()` for virtual time control, which is only available in Debug builds.

### etcd Tests

Tests in `atapp_etcd_cluster` and `atapp_etcd_module` groups require a running etcd instance.

#### Quick Start with setup-etcd Scripts

The `ci/etcd/` directory provides scripts to automatically download, start, and manage a local etcd for testing:

**Windows (PowerShell):**

```powershell
# Download and start etcd (auto-downloads latest if not present)
.\ci\etcd\setup-etcd.ps1 -Command start

# Set env var for test discovery (default port is 12379)
$env:ATAPP_UNIT_TEST_ETCD_HOST = "http://127.0.0.1:12379"

# Run etcd tests
./atapp_unit_test.exe -r atapp_etcd_cluster
./atapp_unit_test.exe -r atapp_etcd_module

# Stop etcd when done
.\ci\etcd\setup-etcd.ps1 -Command stop
```

**Linux / macOS (Bash):**

```bash
# Download and start etcd
bash ci/etcd/setup-etcd.sh start

# Set env var
export ATAPP_UNIT_TEST_ETCD_HOST="http://127.0.0.1:12379"

# Run etcd tests
./atapp_unit_test -r atapp_etcd_cluster
./atapp_unit_test -r atapp_etcd_module

# Stop etcd when done
bash ci/etcd/setup-etcd.sh stop
```

**Available commands:** `download`, `start`, `stop`, `cleanup` (stop + delete all), `status`

**Options:** `--work-dir DIR`, `--client-port PORT` (default: 12379), `--peer-port PORT` (default: 12380), `--etcd-version VER` (default: latest)

#### Manual etcd Setup

If you already have an etcd instance running, just set the environment variable:

```bash
export ATAPP_UNIT_TEST_ETCD_HOST="http://127.0.0.1:2379"
```

If `ATAPP_UNIT_TEST_ETCD_HOST` is not set, etcd-dependent tests are skipped (not failed).

## Writing tests

Test files are under `test/case/`.

### Minimal example

```cpp
#include <frame/test_macros.h>

CASE_TEST(my_group, my_case) {
    int result = do_something();
    CASE_EXPECT_EQ(0, result);
    CASE_EXPECT_TRUE(some_condition());
}
```

### Multi-node test pattern

For tests involving multiple app instances communicating over atbus:

```cpp
#include <frame/test_macros.h>
#include <atframe/atapp.h>

// 1. Define test YAML config files (test/case/*.yaml)
//    Each node needs unique id, name, and listen address

// 2. Init apps (upstream first for proxy tests)
static std::string get_test_dir() {
    std::string dir = __FILE__;
    // ... resolve to test/case/ directory
    return dir;
}

// 3. Helper: pump event loop until condition
static void pump_until(std::vector<atframework::atapp::app*> &apps,
                       std::function<bool()> cond,
                       std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!cond() && std::chrono::steady_clock::now() < deadline) {
        for (auto *a : apps) {
            a->run_noblock();
        }
        CASE_THREAD_SLEEP_MS(8);
    }
}

// 4. Inject discovery nodes for peer awareness
auto node_info = std::make_shared<etcd_discovery_node>();
node_info->copy_from(protobuf_discovery_info);
etcd_module->get_global_discovery().add_node(node_info);
```

### Debug-only time control

For testing timers, reconnect logic, and timeouts:

```cpp
#if defined(ATFRAMEWORK_ATAPP_ENABLE_MOCK_TIME) || !defined(NDEBUG)
// Jump virtual time forward
auto future = std::chrono::system_clock::now() + std::chrono::seconds(60);
atframework::atapp::app::set_sys_now(future);

// Process timers (no I/O)
app.tick();
#endif
```

**Caveats:**

- `set_sys_now()` is static — affects ALL app instances in the process
- Use `tick()` instead of `run_noblock()` during time-advance to avoid cross-app timer interference
- Set large bus timeouts in test YAML configs (e.g., `first_idle_timeout: 3600`) to prevent atbus idle disconnect
- Only one jiffies timer callback fires per `tick()` when time is jumped; use multiple `set_sys_now()+tick()` rounds for multi-timer scenarios

### Config file resolution

Tests locate their YAML config files relative to `__FILE__`:

```cpp
static std::string get_test_case_dir() {
    std::string ret = __FILE__;
    // Strip filename, keep directory
    auto pos = ret.find_last_of("/\\");
    if (pos != std::string::npos) {
        ret = ret.substr(0, pos);
    }
    return ret;
}

// Use in test:
std::string conf_path = get_test_case_dir() + "/atapp_test_0.yaml";
```
