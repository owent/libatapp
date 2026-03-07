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

## Common test groups

- `atapp_setup`
- `atapp_configure_loader`
- `atapp_connector`
- `atapp_discovery`
- `atapp_message`
- `atapp_worker_pool`

## Writing tests

Test files are under `test/case/`.

Minimal example:

- Include: `frame/test_macros.h`
- Use: `CASE_TEST(group, case)` and `CASE_EXPECT_*` assertions
