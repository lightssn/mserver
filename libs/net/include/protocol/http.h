#ifndef CPP_SIMPLE_WEB_SERVER_HTTP_HPP
#define CPP_SIMPLE_WEB_SERVER_HTTP_HPP
#include "rpc.h"
#include <array>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>
#ifdef WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/epoll.h>
#endif

namespace m::net::http {
using namespace std;
using RpcFuncTable = unordered_map<string, rpc::HandlerType>;
static constexpr string_view default_html_dir = "/var/www/html";
static constexpr string_view default_index_page_name = "index.html";

class Reactor;
enum class ResponseCode : short {
    OK = 200,
    BAD_REQUEST = 400,
    FORBIDDEN = 403,
    NOT_FOUND = 404,
    INTERNAL_SERVER_ERROR = 500
    };

static unordered_map<ResponseCode, string_view> http_code_to_title{
        {ResponseCode::BAD_REQUEST, "Bad Request"},
        {ResponseCode::OK, "OK"},
        {ResponseCode::NOT_FOUND, "Not Found"},
        {ResponseCode::FORBIDDEN, "Forbidden"},
        {ResponseCode::INTERNAL_SERVER_ERROR, "Internal Error"}};

static unordered_map<ResponseCode, string_view> http_code_to_body{
        {
        ResponseCode::BAD_REQUEST,
        "<html><body><h1>400 - Your request has bad syntax or is inherently "
        "impossible to satisfy.</h1></body></html>"
        },
        {
        ResponseCode::NOT_FOUND, "<html><body><h1>404 - The requested file was "
                                 "not fount on this server.</h1></body></html>"
        },
        {
        ResponseCode::FORBIDDEN,
        "<html><body><h1>403 - You do not have permission to get file from this "
        "server.</h1></body></html>"
        },
        {
        ResponseCode::INTERNAL_SERVER_ERROR,
        "<html><body><h1>500 - There was an unusual problem serving the requested "
        "file.</h1></body></html>"
        }};

enum class IOState { OK, PENDING, BAD, KEEP_ALIVE };
struct Request {
    string_view url, version, host, method, content;
    size_t content_length{0};
    bool keep_alive{false};
    };

class RequestParser {
    private:
        Request m_request{};
        size_t m_pivot{0};
        enum class ProtocolState {
            ON_REQUEST_LINE,
            ON_HEADERS,
            ON_CONTENT
            } m_protocol_state{ProtocolState::ON_REQUEST_LINE};
        IOState parse_request_line(string_view);
        IOState parse_header_line(string_view);
        IOState parse_content(string_view);
        pair<IOState, string_view> next_line(string_view);

    public:
        inline const Request &unwrap() {
            return m_request;
            };
        IOState update(string_view);
    };

struct ResponseBuffer {
    ResponseCode code{ResponseCode::OK};
    int file_fd{-1};
    string s{};//响应字符串
    size_t write_index{0};
    ssize_t file_size{0}, file_write_index{0};
    };

class EpollSelector {
    private:
        constexpr static int MAX_EVENT_NUM = 10000;
        array<epoll_event, MAX_EVENT_NUM> m_events{};
        int m_timeout{-1};
        int m_epoll_fd, m_listen_fd;
        size_t m_current_event_num{0}, m_current_event_index{0};
        static int set_nonblocking_fd(int fd);

    public:
        struct Event {
            enum class Tag { CONNECTION, READ, WRITE, CLOSE } tag;
            int fd;
            };

        //将文件描述符注册到epoll，监听其可读和连接关闭事件，并设置为非阻塞模式
        void register_on_listening_lt(int fd) const;
        void register_on_reading(int fd, bool one_shot = true,
                                 bool blocking = false) const;
        void register_timer(int fd);
        void read_again_on(int fd) const;
        void write_again_on(int fd) const;
        // remove and close socket
        void unregister(int fd) const;
        Event get_next_event();

        explicit EpollSelector(int listen_fd, size_t size = 5);
        EpollSelector() = delete;
        ~EpollSelector();
    };

// read -> work -> write
class Handler {
        constexpr static size_t READ_BUFFER_SIZE = 2048;
        sockaddr_in m_addr;
        string m_read_buffer;
        size_t m_read_index;
        RequestParser m_request_parser;
        ResponseBuffer m_response_buffer;//http响应数据
        bool m_keep_alive;
        chrono::steady_clock::time_point m_last_alive_time, m_lazy_current_time;

    public:
        friend class Reactor;
        explicit Handler(chrono::steady_clock::time_point, const sockaddr_in &);
        [[nodiscard]] string get_addr_str() const;
        IOState read(int fd);
        IOState work(string_view html_dir, RpcFuncTable  &);
        IOState write(int fd);
        void clear();
        void update_current_time(chrono::steady_clock::time_point);
    };

class Acceptor {
        int m_listen_fd;
    public:
        [[nodiscard]] tuple<int, struct sockaddr_in> accept() const;
        explicit Acceptor(int fd);
        Acceptor() = default;
    };

class Reactor {
    public:
        constexpr static inline size_t MAX_FD = 65536;
        struct Config {
            string_view ip;
            int port;
            string_view mapping_path = default_html_dir;
            size_t working_thread_num = 4, max_idle_seconds = 30, listen_size = 5, selector_size = 5;
            };
    private:
        vector<shared_ptr<Handler>> m_handlers;
        Config m_config;
        int m_server_fd;//服务端套接字
        RpcFuncTable m_rpc_funcs;//映射表

    public:
        explicit Reactor(const Config &);
        void run();

        ~Reactor();
        template <typename Func>
        //将RPC远程过程调用的函数注册到映射表。将函数名与默认路由前缀组合成 URL，并将其与一个包装后的函数一起插入到映射表中，实现 RPC 函数的注册和调用。并处理 JSON 解析和类型转换的错误情况
        bool rpc_register(string const &name, Func func) {
            auto url = string{rpc::default_route_prefix} + name;//拼接完整url
            if (m_rpc_funcs.find(url) != m_rpc_funcs.end())//如果映射表已存在该url，注册失败，返回false
                return false;

            //lambda函数，处理url
            m_rpc_funcs[url] = [func](string_view content) -> string {
                using namespace MyJson;
                auto j = Json::from_json_text(content);//将lambda输入的content解析为json
                if (!j.has_value())//解析失败时，返回错误信息的json
                    return R"({"rpc_error":"broken client json"})";
                using ArgT = remove_cv_t<remove_reference_t<rpc::FirstArgT<Func>>>;//获取输入func的第一个参数类型
//      static_assert(is_same_v<ArgT, int>);
                auto arg = j.value().to_type<ArgT>();//将解析后的json转为该类型
                if (!arg.has_value())//转换失败时，返回错误信息的json
                    return R"({"rpc_error":"broken client arg"})";
                return Json{func(arg.value())}.to_json_text();//调用func处理并将结果转为json返回
                };
            return true;
            }//rpc_register
    };
} // namespace m::net::http
#endif // CPP_SIMPLE_WEB_SERVER_HTTP_HPP
