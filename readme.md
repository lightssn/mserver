### TODO
OPENMP

###  mserver 
https://github.com/zixfy/SimpleWebServer
https://zhuanlan.zhihu.com/p/662574190
单`Reactor`与工作线程池模型的静态网站服务端，支持长连接复用套接字，定时清除非活跃长连接
### Build
cd /mnt/f/code/mserver/build_u24_rel
mkdir build && cd build && cmake .. && make
### Use 
./mserver ip_address port_number threads_num [index_page_path]
其中`ip_address`为服务器局域网地址，`port_number`为监听端口，`threads_num`为工作线程数，`index_page_path`为网站根目录，默认为`/var/www/html`

### Example
cd /mnt/f/code/mserver/out/build/WSL-GCC-Debug/apps
server/server http://127.0.0.1 1090 5

经`Webbench`测试，设定工作线程数为`10`时，`QPS`可达`5.2w`左右。示例命令：
/mnt/f/codeTest/c++Test/netTest/webBench/webbench  -c 10 -t 5 -2 http://127.0.0.1:1090/

### Others
此外，还在`HTTP`协议之上对远程过程调用(`RPC`)功能提供了最小支持，使用静态反射宏的[序列化方案](https://github.com/zixfy/MyJson)，使用`std::function`进行注册函数多态

服务端函数注册:

```c++
DEF_DATA_CLASS(Arg, (float) a, (int) b)

string f(Arg a) {
  return string {"[Server Add]"} + std::to_string(a.a) + " + " + std::to_string(a.b) + " = " +
         std::to_string(a.b + a.a);
}

auto main(int argc, char *argv[]) -> int {
    // ...
    auto server = net::http::Reactor(config);
    server.rpc_register("echo", [](const std::string &x) {
        return std::string{"[Server Echo] "} + x;
    });
    server.rpc_register("calculate", f);
    server.run();
}
```

客户端远程调用:

```c++
int main() {
    // ...
    using namespace m::net::rpc; 
    Client cli;
    cli.dial("/*server address*/ : /*server ip*/");
    cout << cli.call<string, string>("echo", "i am client, rpc okay").value()
         << endl;

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(1.0, 20.0);

    for (int i = 0; i < 13; ++i) {
        auto res = cli.call<Arg, string>(
                "calculate", Arg{dist(rng), static_cast<int>(dist(rng))});
        if (res.has_value())
            cout << res.value() << '\n';
        else
            cout << "rpc faild\n";
    }
}
```

### Record Tests
全原: 149777


