# etcd Test Reference

Read this file when changing etcd-specific behavior or selecting tests. Re-check the setup scripts and exact test source
before relying on versions, ports, environment variables, filters, fallback endpoints, or skip behavior.

## Testing etcd Integration

### Tests Requiring etcd

Tests in `atapp_etcd_cluster_test.cpp` and `atapp_etcd_module_test.cpp` require a running etcd instance.

#### Quick Start with setup-etcd Scripts

Use the `ci/etcd/setup-etcd` scripts to download and start a local etcd. Pin `--etcd-version` to the user/CI requirement
when reproducibility matters; do not assume the script's mutable default is part of the test contract.

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

The current tests use `ATAPP_UNIT_TEST_ETCD_HOST` when set and otherwise probe `http://127.0.0.1:12379`. If the selected
endpoint is unavailable, the case path reports the missing prerequisite and returns without exercising etcd. Confirm the
actual output/case result and report this as unavailable/skipped integration coverage, not a passing unit substitute.

### Tests Not Requiring etcd

Discovery set and packer tests work without etcd:

```bash
./atapp_unit_test -r atapp_discovery
./atapp_unit_test -r atapp_etcd_packer
```

### In-process discovery tests

For behavior that does not require real etcd, follow the closest current discovery/module-unit test and inject a real
generated discovery object through the production discovery/module API. Build one contract-valid baseline from the
current protobuf and fixture, then vary only the behavior-relevant field. Do not copy placeholder fields, invent a mock
API, or configure an address/label merely to force the current implementation down a passing branch.

### Discovery set unit tests

```bash
./atapp_unit_test -r atapp_discovery
```

Use `atapp_unit_test -l` and current source to discover cases; do not maintain a hand-counted or hand-copied inventory in
this reference.
