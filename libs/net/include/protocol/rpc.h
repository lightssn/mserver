#ifndef CPP_SIMPLE_WEB_SERVER_RPC_HPP
#define CPP_SIMPLE_WEB_SERVER_RPC_HPP
#include "my_json/core.h"
#include "../../net.h"
#include "type_traits"
#include <functional>
#include <string_view>


namespace m::net::rpc::detail {
using namespace std;
template <typename T> struct arg_type;
template <typename R, typename Arg> struct arg_type<R(Arg)> {
    using type = Arg;
    };
// function pointer
template <typename R, typename Arg> struct arg_type<R (*)(Arg)> {
    using type = Arg;
    };

// member function pointer
template <typename C, typename R, typename Arg> struct arg_type<R (C::*)(Arg)> {
    using type = Arg;
    };

// const member function pointer
template <typename C, typename R, typename Arg>
struct arg_type<R (C::*)(Arg) const> {
    using type = Arg;
    };
template <typename F, typename = void> struct first_type {
    using type = typename arg_type<F>::type;
    };
template <typename F>
struct first_type<F, enable_if_t<is_class_v<F>>> {
    using type = typename arg_type<decltype(&F::operator())>::type;
    };
} // namespace m::net::rpc::detail

//封装 RPC 相关的功能客户端实现。客户端可以通过 TCP 连接到服务器，发送 POST 请求调用远程函数，并处理服务器的响应
namespace m::net::rpc {
using namespace std;
static string_view default_route_prefix = "/rpc/";//默认路由前缀
using HandlerType = function<string(string_view)>;
// struct RpcError {};
struct Client {
    private:
    string m_ip;
    int m_port;
    int m_sock_fd;//套接字描述符
    sockaddr_in m_serv_addr{};
    string m_response_buffer;

public:
    bool dial(const string &address);

    //模板方法，用于调用远程函数。它接受函数名和参数，发送请求到服务器，并处理服务器的响应
    template <typename ArgT, typename ResT>
    optional<ResT> call(string const &name, ArgT const &arg) {
        if ((m_sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            cout << "Rpc TCP Socket creation error" << endl;
            return {};
            }
        if (connect(m_sock_fd, (struct sockaddr *)&m_serv_addr,
                    sizeof(m_serv_addr)) < 0) {
            cout << "Rpc Connection failed" << endl;
            return {};
            }
        string req = "POST ";
        req += default_route_prefix;
        req += name;
        req += " HTTP/1.1\r\n";
        auto encoded = MyJson::Json{arg}.to_json_text();
        req += "Content-Length: ";
        req += to_string(encoded.size());
        req += "\r\n\r\n";
        req += encoded;
        ssize_t sent_pivot = 0;
        while (sent_pivot < req.size()) {
            auto adder =
                send(m_sock_fd, req.data() + sent_pivot, req.size() - sent_pivot, 0);
            if (adder <= 0) {
                cout << "Rpc Server Closed\n";
                return {};
                }
            sent_pivot += adder;
            }
        m_response_buffer.resize(1024);
        ssize_t read_pivot = 0;
        while (true) {
            if (read_pivot >= m_response_buffer.size())
                m_response_buffer.resize(int(1.25 * m_response_buffer.size()));
            auto adder = recv(m_sock_fd, m_response_buffer.data() + read_pivot,
                              m_response_buffer.size() - read_pivot, 0);
            if (adder < 0) {
                cout << "Rpc Server Closed\n";
                return {};
                }
            else if (adder == 0)
                break;
            read_pivot += adder;
            }
        m_response_buffer.resize(read_pivot);
        size_t rep_begin = 0;
        while (rep_begin < m_response_buffer.size()) {
            if (rep_begin + 3 < m_response_buffer.size() &&
                    string_view{m_response_buffer.data() + rep_begin, 4} ==
                    "\r\n\r\n") {

                break;
                }
            ++rep_begin;
            }
        if (rep_begin + 3 >= m_response_buffer.size()) {

            cout << "Rpc bad response:\n " << m_response_buffer;
            return {};
            }
        auto content = string_view{m_response_buffer.data() + rep_begin + 4,
                                   m_response_buffer.size() - rep_begin - 4};
        auto j = MyJson::Json::from_json_text(content);
        if (!j.has_value()) {
            cout << "Rpc bad response:\n "<< m_response_buffer;
            return {};
            }
        auto decoded = j->to_type<ResT>();
        if (decoded.has_value())
            return decoded.value();
        cout << "Rpc bad response:\n " << m_response_buffer;
        return {};
        }//call
    };
template <typename F> using FirstArgT = typename detail::first_type<F>::type;
} // namespace m::net::rpc
#endif // CPP_SIMPLE_WEB_SERVER_RPC_HPP
