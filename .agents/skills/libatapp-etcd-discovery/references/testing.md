# etcd Test Reference

Read this file when changing etcd-specific behavior or selecting tests. Re-check the setup scripts and test source
before relying on ports, environment variables, filters, or skip behavior.

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
