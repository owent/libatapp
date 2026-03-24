# libatapp

用于搭建高性能、全异步(a)的跨平台应用框架库。

[![ci-badge]][ci-link]
[![codeql-badge]][codeql-link]
[![codecov-badge]][codecov-link]
![license-badge]

[ci-badge]: https://github.com/atframework/libatapp/actions/workflows/main.yml/badge.svg "Github action build status"
[ci-link]:  https://github.com/atframework/libatapp/actions/workflows/main.yml "Github action build status"
[codeql-badge]: https://github.com/atframework/libatapp/actions/workflows/codeql.yml/badge.svg "CodeQL"
[codeql-link]:  https://github.com/atframework/libatapp/actions/workflows/codeql.yml "CodeQL"
[codecov-badge]: https://codecov.io/gh/owent/libatapp/branch/main/graph/badge.svg
[codecov-link]: https://codecov.io/gh/owent/libatapp
[license-badge]: https://img.shields.io/github/license/atframework/libatapp

## CI Job Matrix

| Target System | Toolchain          | Note                  |
| ------------- | ------------------ | --------------------- |
| Linux         | GCC                |
| Linux         | Clang              | With libc++           |
| Windows       | Visual Studio 2022 | Dynamic linking       |
| Windows       | Visual Studio 2022 | Static linking        |
| macOS         | AppleClang         | With libc++           |

## 依赖

+ 支持c++0x或c++11的编译器(为了代码尽量简洁,特别是少做无意义的平台兼容，依赖部分 C11和C++11的功能，所以不支持过低版本的编译器)
  > + GCC: 7.1 及以上
  > + Clang: 7 及以上
  > + VC: VS2022 及以上

+ [cmake](https://cmake.org/download/) 3.24.0 以上

## Clangd 配置参考

本仓库默认使用 `build_jobs_cmake_tools` 作为编译数据库目录（由 CMake 生成 `compile_commands.json`）。建议保持与当前设置一致：

**.clangd（可选）**

```yaml
CompileFlags:
    CompilationDatabase: build_jobs_cmake_tools

Index:
    Background: Build

Diagnostics:
    UnusedIncludes: Strict
```

**VS Code settings**

```json
{
    "C_Cpp.intelliSenseEngine": "disabled",
    "clangd.enable": true,
    "clangd.arguments": [
        "--compile-commands-dir=${workspaceFolder}/build_jobs_cmake_tools",
        "--background-index",
        "--clang-tidy",
        "--completion-style=detailed",
        "--header-insertion=iwyu",
        "-j=8"
    ]
}
```

**MSVC + clangd --query-driver 推荐配置**

当使用 MSVC 工具链时，建议显式配置 `--query-driver` 以便 clangd 正确读取 MSVC 的系统头与内置宏：

```jsonc
{
    "clangd.arguments": [
        // 使用环境变量（推荐，VS 开发者命令行会注入 VCToolsInstallDir）
        "--query-driver=${env:VCToolsInstallDir}bin/Hostx64/x64/cl.exe",

        // 通配符版本（多个版本时会选择第一个匹配项）
        "--query-driver=C:/Program Files/Microsoft Visual Studio/*/Community/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"
    ]
}
```

**显式指定 C++ 标准（CMake 配置）**

如果需要固定 `__cplusplus` 版本，可通过 CMake 统一指定标准（示例：C++20）：

```jsonc
{
    "cmake.configureSettings": {
        "CMAKE_CXX_STANDARD": "20",
        "CMAKE_CXX_STANDARD_REQUIRED": "ON"
    }
}
```

说明：只有在 MSVC 下 clangd 不识别 `-std:c++latest` 和 `/std:c++latest` 时，才需要使用以上方式固定标准。使用 `CMAKE_CXX_STANDARD` 通常兼容性更好；`--query-driver` 不影响单独打开的 `.h` 文件，仅影响带编译命令的翻译单元。两者可二选一使用。

## GET START

### 最小化服务器

```cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include <uv.h>

#include <atframe/atapp.h>
#include <common/file_system.h>

static int app_handle_on_msg(atframework::atapp::app &app, const atframework::atapp::app::message_sender_t& source, const atframework::atapp::app::message_t &msg) {
    std::string data;
    data.assign(reinterpret_cast<const char *>(msg.data), msg.data_size);
    FWLOGINFO("receive a message(from {:#x}, type={}) {}", source.id, msg.type, data);

    return app.get_bus_node()->send_data(source.id, msg.type, msg.data, msg.data_size);
}

static int app_handle_on_response(atframework::atapp::app & app, const atframework::atapp::app::message_sender_t& source, const atframework::atapp::app::message_t & msg, int32_t error_code) {
    if (error_code < 0) {
        FWLOGERROR("send data from {:#x} to {:#x} failed, sequence: {}, code: {}", app.get_id(), source.id, msg.msg_sequence, error_code);
    } else {
        FWLOGDEBUG("send data from {:#x} to {:#x} got response, sequence: {}", app.get_id(), source.id, msg.msg_sequence);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    atframework::atapp::app app;

    // 设置工程目录，不设置的话日志打印出来都是绝对路劲，比较长
    {
        std::string proj_dir;
        atfw::util::file_system::dirname(__FILE__, 0, proj_dir, 2); // 设置当前源文件的2级父目录为工程目录
        atfw::util::log::log_formatter::set_project_directory(proj_dir.c_str(), proj_dir.size());
    }

    // setup handle
    app.set_evt_on_forward_request(app_handle_on_msg);         // 注册接收到数据后的回调
    app.set_evt_on_forward_response(app_handle_on_response);   // 注册发送消息失败的回调

    // run with default loop in libuv
    return app.run(uv_default_loop(), argc, (const char **)argv, nullptr);
}
```

### 设置自定义命令行参数

```cpp
// ...

#include <sstream>

static int app_option_handler_echo(atfw::util::cli::callback_param params) {
    // 获取参数并输出到stdout
    std::stringstream ss;
    for (size_t i = 0; i < params.get_params_number(); ++i) {
        ss << " " << params[i]->to_cpp_string();
    }

    std::cout << "echo option: " << ss.str() << std::endl;
    return 0;
}

int main(int argc, char *argv[]) {
    atframework::atapp::app app;
    // ...
    // setup options, 自定义命令行参数是区分大小写的
    atfw::util::cli::cmd_option::ptr_type opt_mgr = app.get_option_manager();
    // show help and exit
    opt_mgr->bind_cmd("-echo", app_option_handler_echo)   // 当启动参数里带-echo时跳转进 app_option_handler_echo 函数
        ->set_help_msg("-echo [text]                           echo a message."); // 帮助文本，--help时显示，不执行这个就没有帮助信息

    // ...
    // run
    return app.run(uv_default_loop(), argc, (const char **)argv, nullptr);
}
```

### 设置自定义远程命令

```cpp
// ...
#include <sstream>

static int app_command_handler_echo(atfw::util::cli::callback_param params) {
    std::stringstream ss;
    for (size_t i = 0; i < params.get_params_number(); ++i) {
        ss << " " << params[i]->to_cpp_string();
    }

    WLOGINFO("echo commander:%s", ss.str().c_str());
    return 0;
}

int main(int argc, char *argv[]) {
    atframework::atapp::app app;
    // ... 
    // setup cmd, 自定义远程命令是不区分大小写的
    atfw::util::cli::cmd_option_ci::ptr_type cmgr = app.get_command_manager();
    cmgr->bind_cmd("echo", app_command_handler_echo)
        ->set_help_msg("echo       [messages...]                                    echo messages to log");

    // 然后就可以通过 [EXECUTABLE] -id ID --conf CONFIGURE_FILE run echo MESSAGES ... 来发送命令到正在运行的服务器进程了
    // ...

    // run
    return app.run(uv_default_loop(), argc, (const char **)argv, nullptr);
}
```

### 自定义模块

```cpp
// ...

// 自定义模块必需继承自atapp::module_impl
class echo_module : public atapp::module_impl {
public:
    virtual int init() {
        // 初始化时调用，整个app的生命周期只会调用一次
        // 初始化时这个调用再reload只会，这样保证再init的时候配置时可用的
        WLOGINFO("echo module init");
        return 0;
    };

    virtual int reload() {
        // 重新加载配置时调用
        WLOGINFO("echo module reload");
        return 0;
    }

    virtual int stop() {
        // app即将停止时调用，返回非0值表示需要异步回收数据，这时候等回收完成后需要手动再次调用atapp的stop函数
        WLOGINFO("echo module stop");
        return 0;
    }

    virtual int timeout() {
        // stop超时后调用，这个返回以后这个模块会被强制关闭
        WLOGINFO("echo module timeout");
        return 0;
    }

    virtual const char *name() const { 
        // 返回模块名，如果不重载会尝试使用C++ RTTI特性判定，但是RTTI生成的符号名称可能不是很易读
        return "echo_module"; 
    }

    virtual int tick() {
        // 每次tick的时候调用，tick间隔由配置文件指定，返回成功执行的任务数
        time_t cur_print = atfw::util::time::time_utility::get_sys_now() / 20;
        static time_t print_per_sec = cur_print;
        if (print_per_sec != cur_print) {
            WLOGINFO("echo module tick");
            print_per_sec = cur_print;
        }

        // 返回值大于0时，atapp会认为模块正忙，会很快再次调用tick
        // 这样可以阻止atapp进入sleep
        return 0;
    }
};

int main(int argc, char *argv[]) {
    atframework::atapp::app app;
    // ... 
    // setup module, 自定义模块必需是shared_ptr
    app.add_module(std::make_shared<echo_module>());
    // ...

    // run
    return app.run(uv_default_loop(), argc, (const char **)argv, nullptr);
}
```

**更多的细节请参照 [sample](sample)**

## 本地 etcd 测试

部分单元测试（`atapp_etcd_cluster`、`atapp_etcd_module`）需要连接 etcd 服务。`ci/etcd/` 目录下提供了自动下载、启动和管理本地 etcd 的脚本。

### Windows (PowerShell)

```powershell
# 下载并启动 etcd（默认客户端端口 12379）
.\ci\etcd\setup-etcd.ps1 -Command start

# 设置环境变量，让单元测试发现 etcd
$env:ATAPP_UNIT_TEST_ETCD_HOST = "http://127.0.0.1:12379"

# 运行 etcd 相关测试
./atapp_unit_test.exe -r atapp_etcd_cluster
./atapp_unit_test.exe -r atapp_etcd_module

# 停止 etcd
.\ci\etcd\setup-etcd.ps1 -Command stop

# 完全清理（停止 + 删除二进制和数据）
.\ci\etcd\setup-etcd.ps1 -Command cleanup
```

### Linux / macOS (Bash)

```bash
bash ci/etcd/setup-etcd.sh start
export ATAPP_UNIT_TEST_ETCD_HOST="http://127.0.0.1:12379"
./atapp_unit_test -r atapp_etcd_cluster
./atapp_unit_test -r atapp_etcd_module
bash ci/etcd/setup-etcd.sh stop
```

### 可用命令

| 命令        | 说明                           |
| ----------- | ------------------------------ |
| `download`  | 仅下载 etcd 二进制             |
| `start`     | 下载（如需）并启动 etcd        |
| `stop`      | 停止 etcd                      |
| `cleanup`   | 停止并删除所有二进制和数据     |
| `status`    | 检查 etcd 运行状态和健康状况   |

### 可选参数

| 参数 (PS1)      | 参数 (sh)          | 默认值                  | 说明               |
| --------------- | ------------------ | ----------------------- | ------------------ |
| `-WorkDir`      | `--work-dir`       | `$TEMP/etcd-unit-test`  | 工作目录           |
| `-ClientPort`   | `--client-port`    | `12379`                 | 客户端监听端口     |
| `-PeerPort`     | `--peer-port`      | `12380`                 | 节点间通信端口     |
| `-EtcdVersion`  | `--etcd-version`   | `latest`                | etcd 版本号        |

如果已有 etcd 服务运行，只需设置环境变量 `ATAPP_UNIT_TEST_ETCD_HOST` 即可。未设置时 etcd 相关测试将被跳过。
