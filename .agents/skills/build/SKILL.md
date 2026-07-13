---
name: build
description: "Use when: configuring or building libatapp with CMake, editing or reviewing CMake generation/dependency rules, changing shared/static builds, or adjusting build type options."
---

# Build (libatapp)

This repo uses **CMake (>= 3.24)** and requires C++17.

## Workspace CMake Settings

When `libatapp` is built inside a larger workspace, read the active workspace `.vscode/settings.json` before running
`cmake` and reuse its generator, configure settings, build directory, and parallel-job settings. In the current
atsf4g-co checkout, that means the Ninja/Debug/unit-test-enabled build tree at `build_jobs_cmake_tools` unless the user
asks for a different configuration.

## Typical build flow

- Configure: `cmake ..`
- Build:
  - Linux/macOS: `cmake --build .`
  - Windows (MSVC): `cmake --build . --config RelWithDebInfo`

## Incremental build stability

- Treat unconditional `touch` or same-content overwrites of code/resources consumed by `add_custom_command`,
  `add_custom_target`, `add_executable`, `add_library`, or `target_sources` as a blocking defect, including generated,
  copied, and other non-handwritten files.
- Declare real `OUTPUT`/`BYPRODUCTS` and accurate `DEPENDS`/`DEPFILE`; publish content-stably with
  `configure_file`, `file(CONFIGURE)`, `file(GENERATE)`, or a temporary file plus `cmake -E copy_if_different`.
- Use a dedicated stamp/witness only when it is not itself compiled, linked, packaged, installed, or substituted for a
  real output/byproduct.

## Run tests via CTest

- `ctest . -V`

## Key CMake options

- `BUILD_SHARED_LIBS` (NO/YES)
- `CMAKE_BUILD_TYPE` (Debug/Release/RelWithDebInfo)
