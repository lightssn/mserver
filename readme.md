# C++17 RPC & Static Web Server
c++17跨平台http静态服务器，有基于post+json的轻量级rpc能力

#架构
服务端：单Reactor事件循环+工作线程池
	主线程通过异步I/O api（win iocp/linux epoll）监听连接、收发事件、管理连接生命周期
	将业务逻辑（解析请求、执行rpc函数、生成响应）分发到线程池异步处理
	主线程注册写事件并发送响应
客户端：建立tcp连接、构造http post请求、发送json、解析json响应

#模块
apps
	server 服务端，配置监听地址/端口/线程数/静态目录、注册rpc函数
	client 客户端
libs
	net socket、http、rpc、epoll/iocp、定时器、json
	thread 线程池
	net
		json 解析器
		fmt 格式化库

#功能
http：keep-alive、定时清除非活跃连接

#Build
apt install libgtest-dev libgmock-dev
cd /mnt/f/code/mserver/build_u24_rel
mkdir build && cd build && cmake .. && make

#Use 
./mserver ip_address port_number threads_num [index_page_path]
其中`ip_address`为服务器局域网地址，`port_number`为监听端口，`threads_num`为工作线程数，`index_page_path`为网站根目录，默认为`/var/www/html`

### Example
cd F:\code\mserver\out\build\x64-Debug\apps\server
cd /mnt/f/code/mserver/out/build/WSL-GCC-Release/apps/server
./server http://127.0.0.1 8888 5

curl -X POST http://127.0.0.1:8888/rpc/echo -d '"hello world"' -H "Content-Type: application/json"

curl -X POST http://127.0.0.1:8888/rpc/calculate -d '{"a":1.5,"b":2}' -H "Content-Type: application/json"

经`Webbench`测试，设定工作线程数为`10`时，`QPS`可达`5.2w`左右。示例命令：
cd /mnt/f/codeTest/c++Test/netTest/webbench
./webbench  -c 10 -t 5 -2 http://127.0.0.1:8888/
(https://github.com/EZLippi/WebBench)

### Others
此外，还在`HTTP`协议之上对远程过程调用(`RPC`)功能提供了最小支持，使用静态反射宏的[序列化方案](https://github.com/zixfy/MyJson)，使用`std::function`进行注册函数多态

#是否要内存池
不需要，瓶颈在网络和磁盘，现有make_shared足够

#TODO
Windows IOCP 路径仍有临时逻辑，当前写事件分支会关闭连接，完整的异步发送流程尚未完成？
RPC 目前主要支持单参数函数，并依赖 JSON 转换；异常处理、错误协议和请求校验仍较简单
复用 HTTP 解析缓冲区（`vector::clear()` 重用
确保线程池提交的 lambda 捕获量小于 `std::function` SBO 阈值（~32B）
研究rpc_register应该怎么解析json
合并两个服务器
实现线程亲和性
实现tcp udp https

BUG
公司电脑监听8080失败，未知

win iocp
OPENMP

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

### Test
cd build && ctest
or ./libs/thread/thread_pool_test

home 1811376
work 1780632

# Reference
https://github.com/zixfy/SimpleWebServer
https://zhuanlan.zhihu.com/p/662574190
