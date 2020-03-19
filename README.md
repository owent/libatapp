# libatapp

基于[libatbus](https://github.com/atframework/libatbus)（树形结构进程间通信库）的服务器应用框架库

> Build & Run Unit Test in |  Linux+OSX(clang+gcc) | Windows+MinGW(vc+gcc) |
> -------------------------|--------|---------|
> Status |  暂未配置CI | 暂未配置CI |
> Compilers | linux-gcc-4.4 <br /> linux-gcc-4.6 <br /> linux-gcc-4.9 <br /> linux-gcc-6 <br /> linux-clang-3.5 <br /> osx-apple-clang-6.0 <br /> | MSVC 12(Visual Studio 2013) <br /> MSVC 14(Visual Studio 2015) <br />Mingw32-gcc <br /> Mingw64-gcc
>

Gitter
------
[![Gitter](https://badges.gitter.im/atframework/common.svg)](https://gitter.im/atframework/common?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge)

依赖
------

+ 支持c++0x或c++11的编译器(为了代码尽量简洁,特别是少做无意义的平台兼容，依赖部分 C11和C++11的功能，所以不支持过低版本的编译器)
> + GCC: 4.4 及以上（建议gcc 4.8.1及以上）
> + Clang: 3.7 及以上
> + VC: 12 及以上 （建议VC 14及以上）

+ [cmake](https://cmake.org/download/) 3.7.0 以上
+ [msgpack](https://github.com/msgpack/msgpack-c)（用于协议打解包,仅使用头文件）
+ [libuv](http://libuv.org/)（用于网络通道）
+ [atframe_utils](https://github.com/atframework/atframe_utils)（基础公共代码）
+ [libatbus](https://github.com/atframework/libatbus)（树形结构进程间通信库）
+ [libiniloader](https://github.com/owt5008137/libiniloader)（ini读取）

GET START
------
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

static int app_handle_on_msg(atapp::app &app, const atapp::app::msg_t &msg, const void *buffer, size_t len) {
    std::string data;
    data.assign(reinterpret_cast<const char *>(buffer), len);
    WLOGINFO("receive a message(from 0x%llx, type=%d) %s", static_cast<unsigned long long>(msg.head.src_bus_id), msg.head.type, data.c_str());

    if (NULL != msg.body.forward && 0 != msg.body.forward->from) {
        // echo server 调用发送接口发回
        return app.get_bus_node()->send_data(msg.body.forward->from, msg.head.type, buffer, len);
    }

    return 0;
}

static int app_handle_on_send_fail(atapp::app &app, atapp::app::app_id_t src_pd, atapp::app::app_id_t dst_pd, const atbus::protocol::msg &m) {
    WLOGERROR("send data from 0x%llx to 0x%llx failed", static_cast<unsigned long long>(src_pd), static_cast<unsigned long long>(dst_pd));
    return 0;
}

int main(int argc, char *argv[]) {
    atapp::app app;

    // 设置工程目录，不设置的话日志打印出来都是绝对路劲，比较长
    {
        std::string proj_dir;
        util::file_system::dirname(__FILE__, 0, proj_dir, 2); // 设置当前源文件的2级父目录为工程目录
        util::log::log_formatter::set_project_directory(proj_dir.c_str(), proj_dir.size());
    }

    // setup handle
    app.set_evt_on_recv_msg(app_handle_on_msg);         // 注册接收到数据后的回掉
    app.set_evt_on_forward_response(app_handle_on_send_fail);  // 注册发送消息失败的回掉

    // run with default loop in libuv
    return app.run(uv_default_loop(), argc, (const char **)argv, NULL);
}
```

### 设置自定义命令行参数
```cpp
// ...

#include <sstream>

static int app_option_handler_echo(util::cli::callback_param params) {
    // 获取参数并输出到stdout
    std::stringstream ss;
    for (size_t i = 0; i < params.get_params_number(); ++i) {
        ss << " " << params[i]->to_cpp_string();
    }

    std::cout << "echo option: " << ss.str() << std::endl;
    return 0;
}

int main(int argc, char *argv[]) {
    atapp::app app;
    // ...
    // setup options, 自定义命令行参数是区分大小写的
    util::cli::cmd_option::ptr_type opt_mgr = app.get_option_manager();
    // show help and exit
    opt_mgr->bind_cmd("-echo", app_option_handler_echo)   // 当启动参数里带-echo时跳转进 app_option_handler_echo 函数
        ->set_help_msg("-echo [text]                           echo a message."); // 帮助文本，--help时显示，不执行这个就没有帮助信息

    // ...
    // run
    return app.run(uv_default_loop(), argc, (const char **)argv, NULL);
}
```

### 设置自定义远程命令
```cpp
// ...
#include <sstream>

static int app_command_handler_echo(util::cli::callback_param params) {
    std::stringstream ss;
    for (size_t i = 0; i < params.get_params_number(); ++i) {
        ss << " " << params[i]->to_cpp_string();
    }

    WLOGINFO("echo commander:%s", ss.str().c_str());
    return 0;
}

int main(int argc, char *argv[]) {
    atapp::app app;
    // ... 
    // setup cmd, 自定义远程命令是不区分大小写的
    util::cli::cmd_option_ci::ptr_type cmgr = app.get_command_manager();
    cmgr->bind_cmd("echo", app_command_handler_echo)
        ->set_help_msg("echo       [messages...]                                    echo messages to log");

    // 然后就可以通过 [EXECUTABLE] -id ID --conf CONFIGURE_FILE run echo MESSAGES ... 来发送命令到正在运行的服务器进程了
    // ...

    // run
    return app.run(uv_default_loop(), argc, (const char **)argv, NULL);
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
        time_t cur_print = util::time::time_utility::get_now() / 20;
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
    atapp::app app;
    // ... 
    // setup module, 自定义模块必需是shared_ptr
    app.add_module(std::make_shared<echo_module>());
    // ...

    // run
    return app.run(uv_default_loop(), argc, (const char **)argv, NULL);
}
```

**更多的细节请参照 [sample](sample)**