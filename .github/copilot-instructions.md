# libatapp - Copilot Instructions

## Project Overview

**libatapp** is a high-performance, fully asynchronous cross-platform application framework library. It provides a standardized way to build server applications with support for configuration management, module systems, service discovery (etcd), and message bus integration.

- **Repository**: https://github.com/atframework/libatapp
- **License**: MIT
- **Languages**: C++ (C++17 required, C++17/C++20/C++23 features used when available)

## Skills (How-to playbooks)

Operational, copy/paste-friendly guides live in `.github/skills/`:

- Entry point: `.github/skills/README.md`

## Build System

This project uses **CMake** (minimum version 3.24.0).

Build steps and common configuration options are documented in:

- `.github/skills/build.md`

## Directory Structure

```
libatapp/
├── include/               # Public headers
│   └── atframe/           # Main include directory
│       ├── atapp.h        # Main application class
│       ├── atapp_conf.h   # Configuration structures
│       ├── atapp_conf.proto  # Protobuf config definition
│       ├── atapp_module_impl.h     # Module interface
│       ├── atapp_log_sink_maker.h  # Log sink factory
│       ├── connectors/    # Connector implementations
│       │   ├── atapp_connector_impl.h    # Base connector
│       │   ├── atapp_connector_atbus.h   # atbus connector
│       │   ├── atapp_connector_loopback.h # Loopback connector
│       │   └── atapp_endpoint.h          # Endpoint abstraction
│       ├── etcdcli/       # etcd client
│       └── modules/       # Built-in modules
│           ├── etcd_module.h         # Service discovery
│           ├── worker_pool_module.h  # Worker pool
│           └── worker_context.h      # Worker context
├── src/                   # Implementation files
│   ├── atframe/           # App implementation
│   └── log/               # Log implementation
├── test/                  # Unit tests
│   └── case/              # Test case files
├── sample/                # Sample applications
├── binding/               # Language bindings
└── tools/                 # Utility tools
```

## Unit Testing Framework

This project uses the **same private unit testing framework** as atframe_utils and libatbus (not Google Test).

### Test Framework Macros

```cpp
// Define a test case
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

### Running and writing tests

See `.github/skills/testing.md`.

## Key Components

### Application (`atapp.h`)

The main application class that manages the application lifecycle.

```cpp
#include <atframe/atapp.h>

int main(int argc, char *argv[]) {
    atframework::atapp::app app;

    // Setup event handlers
    app.set_evt_on_forward_request(my_message_handler);
    app.set_evt_on_forward_response(my_response_handler);

    // Run the application
    return app.run(uv_default_loop(), argc, (const char**)argv, nullptr);
}
```

### Message Handler

```cpp
static int app_handle_on_msg(
    atframework::atapp::app &app,
    const atframework::atapp::app::message_sender_t& source,
    const atframework::atapp::app::message_t &msg
) {
    // Process received message
    std::string data(reinterpret_cast<const char*>(msg.data), msg.data_size);
    FWLOGINFO("Received message from {:#x}, type={}", source.id, msg.type);

    // Echo back
    return app.get_bus_node()->send_data(source.id, msg.type, msg.data, msg.data_size);
}
```

### Custom Command-Line Options

```cpp
int main(int argc, char *argv[]) {
    atframework::atapp::app app;

    // Get option manager
    auto opt_mgr = app.get_option_manager();

    // Bind custom option
    opt_mgr->bind_cmd("-echo", app_option_handler_echo)
        ->set_help_msg("-echo [text]    echo a message.");

    return app.run(uv_default_loop(), argc, (const char**)argv, nullptr);
}
```

### Custom Remote Commands

```cpp
int main(int argc, char *argv[]) {
    atframework::atapp::app app;

    // Get command manager (case-insensitive)
    auto cmgr = app.get_command_manager();

    // Bind custom command
    cmgr->bind_cmd("echo", app_command_handler_echo)
        ->set_help_msg("echo [messages...]    echo messages to log");

    return app.run(uv_default_loop(), argc, (const char**)argv, nullptr);
}

// Usage: ./myapp -id ID --conf CONFIG_FILE run echo MESSAGE...
```

### Custom Modules

Modules provide modular functionality with lifecycle hooks:

```cpp
#include <atframe/atapp_module_impl.h>

class my_module : public atframework::atapp::module_impl {
public:
    // Called once during initialization (after config reload)
    virtual int init() override {
        WLOGINFO("my_module init");
        return 0;
    }

    // Called when configuration is reloaded
    virtual int reload() override {
        WLOGINFO("my_module reload");
        return 0;
    }

    // Called when app is stopping
    // Return non-zero for async cleanup (call stop() again when done)
    virtual int stop() override {
        WLOGINFO("my_module stop");
        return 0;
    }

    // Called after stop timeout
    virtual int timeout() override {
        WLOGINFO("my_module timeout");
        return 0;
    }

    // Module name (overrides RTTI-based name)
    virtual const char* name() const override {
        return "my_module";
    }

    // Called every tick (interval from config)
    // Return >0 to indicate busy (prevents sleep)
    virtual int tick() override {
        // Do periodic work
        return 0;
    }
};

// Register module
int main(int argc, char *argv[]) {
    atframework::atapp::app app;
    app.add_module(std::make_shared<my_module>());
    return app.run(uv_default_loop(), argc, (const char**)argv, nullptr);
}
```

### Built-in Modules

- **etcd_module** (`modules/etcd_module.h`) - Service discovery via etcd
- **worker_pool_module** (`modules/worker_pool_module.h`) - Worker thread pool

### Connectors

- **atapp_connector_impl** - Base connector interface
- **atapp_connector_atbus** - libatbus message bus connector
- **atapp_connector_loopback** - Loopback connector for testing

## Configuration

Configuration is defined in `atapp_conf.proto` and can be loaded from:

- YAML files (`.yaml`)
- JSON files
- Environment variables
- Command-line arguments

Example configuration file (`app.yaml`):

```yaml
id: 0x12345678
name: "my_service"
bus:
  listen:
    - "ipv4://0.0.0.0:12345"
log:
  level: info
```

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

## Code Formatting

This project uses **clang-format** for code formatting. The `.clang-format` file is located at the project root.

- Style: Based on Google style
- Column limit: 120
- Run formatting: `clang-format -i <file>`

## Coding Conventions

1. **Namespace**: `atframework::atapp`
2. **Include guards**: Use `#pragma once`
3. **C++ Standard**: C++17 required
4. **Naming**:
   - Classes/structs: `snake_case`
   - Functions: `snake_case`
   - Types: `*_t` suffix for typedefs
5. **Smart pointers**: Use `std::shared_ptr` for modules and app components

## Compiler Support

| Compiler | Minimum Version |
| -------- | --------------- |
| GCC      | 7.1+            |
| Clang    | 7+              |
| MSVC     | VS2022+         |

## Dependencies

- **atframe_utils** - Core utility library
- **libatbus** - Message bus
- **libuv** - Async I/O event loop
- **protobuf** - Configuration serialization
- **yaml-cpp** (optional) - YAML configuration support
- **etcd client** (optional) - Service discovery

## Application Lifecycle

1. **Parse arguments** - Command-line options
2. **Load configuration** - From file/env/args
3. **Initialize modules** - `module::init()` called
4. **Start event loop** - Main loop begins
5. **Tick modules** - Periodic `module::tick()` calls
6. **Handle messages** - Process incoming messages
7. **Stop** - `module::stop()` called
8. **Cleanup** - Resources released

## Sample Applications

See the `sample/` directory for complete examples:

- Minimal server
- Echo server with custom commands
- Multi-module application
