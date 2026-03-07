# Build (libatapp)

This repo uses **CMake (>= 3.24)** and requires C++17.

## Typical build flow

- Configure: `cmake ..`
- Build:
  - Linux/macOS: `cmake --build .`
  - Windows (MSVC): `cmake --build . --config RelWithDebInfo`

## Run tests via CTest

- `ctest . -V`

## Key CMake options

- `BUILD_SHARED_LIBS` (NO/YES)
- `CMAKE_BUILD_TYPE` (Debug/Release/RelWithDebInfo)
