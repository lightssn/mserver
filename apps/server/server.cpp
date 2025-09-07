#include <csignal>
#include <fmt/core.h>
#include <iostream>
#include <stdexcept>
#include <protocol/http.h>
#include <protocol/tcp.h>
#include <os.h>
#include <test_type.h>
#include "../../libs/net/server.h"
#include "../../libs/net/iocpserver.h"
using namespace m;
using namespace std::chrono_literals;
using namespace MyTypeList;
using namespace std;

//传入自定义类型，返回带[Server Add]前缀的成员变量a和b值及求和的拼接结果
string fun(Arg a) {
    return string {"[Server Add]"} + to_string(a.a) + " + " + to_string(a.b) + " = " + to_string(a.b + a.a);
    //使用ostringstream减少内存分配和复制操作
    //ostringstream oss;
    //oss << "[Server Add]" << a.a << " + " << a.b << " = " << (a.b + a.a);
    //return oss.str();
    }

int main(int argc, char *argv[]) {
    //解析参数
    string_view html_root_dir = net::http::default_html_dir;
    const char *ip;
    int port;
    size_t working_thread_num;
#ifndef NDEBUG
    ip = "http://127.0.0.1";
    port = 8080;
    working_thread_num = 5;
#else
    try {
        if (argc < 4 || argc > 5)
            throw invalid_argument{ "bad argc" };
        ip = argv[1];
        port = stoi(argv[2]);
        working_thread_num = stoi(argv[3]);
        if (argc > 4)
            html_root_dir = argv[4];
    }
    catch (...) {
        fmt::print(
            "usage: {} ip_address port_number threads_num [index_page_path]\n",
            argv[0]);
        return 0;
    }
#endif

    //m::os::handle_signal(SIGPIPE, SIG_IGN);

    //mnet::Server mserver(0000, "tcp");


    //创建并配置基于http的rpc服务器
#ifdef WIN32
//#if 0
    try {
        IOCPServer server(port);
        //捕获Ctrl+C，完成I/O再退出
        SetConsoleCtrlHandler([](DWORD dwCtrlType) -> BOOL {
            if (dwCtrlType == CTRL_C_EVENT) { exit(0); }
            return TRUE;
            }, TRUE);
        //注册rpc服务，服务名echo，处理函数为lambda函数：传入字符串，返回带[Server Echo]前缀的字符串
        server.rpc_register("echo", [](const string& x) {
            return string{ "[Server Echo] " } + x;
            });
        //注册rpc服务，服务名calculate，处理函数为fun
        server.rpc_register("calculate", fun);
        server.run();
        safe_print("Server has been shut down.");
    }
    catch (const exception& e) {
        cerr << "IOCPServer error: " << e.what() << endl;
        return 1;
    }
#else
    net::http::Config config{ ip,
                                      port,
                                      html_root_dir,//html根目录
                                      working_thread_num,//工作线程数
                                      30,//连接最大空闲秒数
                                      5//监听队列大小，队列中等待处理的客户端连接最大数量
    };
    try {
        net::http::Reactor server = net::http::Reactor(config);
        //注册rpc服务，服务名echo，处理函数为lambda函数：传入字符串，返回带[Server Echo]前缀的字符串
        server.rpc_register("echo", [](const string& x) {
            return string{ "[Server Echo] " } + x;
            });
        //注册rpc服务，服务名calculate，处理函数为fun
        server.rpc_register("calculate", fun);
        server.run();//启动服务器
    }
    catch (const exception& e) {
        cerr << "Reactor error: " << e.what() << endl;
        return -1;
    }
#endif
    return 0;
    }//main