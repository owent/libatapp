# libatapp

用于搭建高性能、全异步(a)的跨平台应用框架库。

[![ci-badge]][ci-link]

[ci-badge]: https://github.com/atframework/libatapp/actions/workflows/main.yml/badge.svg "Github action build status"
[ci-link]:  https://github.com/atframework/libatapp/actions/workflows/main.yml "Github action build status"

## CI Job Matrix

| Target System | Toolchain          | Note                  |
| ------------- | ------------------ | --------------------- |
| Linux         | GCC                |
| Linux         | Clang              | With libc++           |
| MinGW64       | GCC                | Dynamic linking       |
| Windows       | Visual Studio 2022 | Dynamic linking       |
| Windows       | Visual Studio 2022 | Static linking        |
| macOS         | AppleClang         | With libc++           |

## 依赖

+ 支持c++0x或c++11的编译器(为了代码尽量简洁,特别是少做无意义的平台兼容，依赖部分 C11和C++11的功能，所以不支持过低版本的编译器)
  > + GCC: 7.1 及以上
  > + Clang: 7 及以上
  > + VC: VS2022 及以上

+ [cmake](https://cmake.org/download/) 3.24.0 以上

## Clangd 配置参考（基于当前 .vscode/settings.json）

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

**VS Code settings（当前仓库默认）**

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
