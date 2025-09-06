#include <algorithm>
#include <fcntl.h>
#include <fmt/core.h>
#include <protocol/http.h>
#include <sstream>
#include <sys/stat.h>
#ifdef WIN32
#include <corecrt_io.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std;
namespace m::net::http {

auto Handler::get_addr_str() const -> string {
    return fmt::format("{}:{}", string_view{inet_ntoa(m_addr.sin_addr)}, ntohs(m_addr.sin_port));
    }

IOState Handler::read(int fd) {
    m_last_alive_time = m_lazy_current_time;//更新活跃时间
    //调整缓冲区大小
    if (m_read_buffer.size() < READ_BUFFER_SIZE)
        m_read_buffer.resize(READ_BUFFER_SIZE);
    if (m_read_index >= READ_BUFFER_SIZE)
        return IOState::BAD;//缓冲区已满，无法继续读取
    while (true) {//循环读取数据
        auto bytes_read = recv(fd, m_read_buffer.data() + m_read_index, READ_BUFFER_SIZE - m_read_index, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)//资源暂不可用(缓冲区已满)
                break;
            return IOState::BAD;
            }
        else if (bytes_read == 0) {//客户端主动断开连接
            // client has closed gracefully :(
            return IOState::BAD;
            }
        m_read_index += bytes_read;//继续读取直到无数据可读或缓冲区满
        }
    return IOState::OK;
    /*潜在问题：如果 m_read_index 接近 READ_BUFFER_SIZE，但未完全填满，后续调用 read() 会直接返回 IOState::BAD（因为 m_read_index >= READ_BUFFER_SIZE），导致数据丢失*/
    }

//传入html根目录、rpc函数映射表，处理 HTTP 请求。首先解析请求内容，判断请求是 RPC（远程过程调用）请求还是普通的文件请求，然后根据请求类型生成相应的 HTTP 响应
//TODO 使用更高效的字符串处理方式
//oth 没有对请求的 URL 进行有效的验证和过滤，可能会导致目录遍历攻击
auto Handler::work(string_view html_dir, RpcFuncTable &rpc_table) -> IOState {//-> IOState是后置返回声明
    //解析读取缓冲区中的请求内容
    auto state = m_request_parser.update(string_view(m_read_buffer.data(), m_read_index));

    if (state == IOState::PENDING)
        return IOState::PENDING;//请求数据不完整，直接返回
    else if (state == IOState::BAD)//请求格式错误
        m_response_buffer.code = ResponseCode::BAD_REQUEST;
    auto &request = m_request_parser.unwrap();//解析后的请求对象的引用

    //处理rpc请求
    if constexpr (true) {
        auto rpc_url = string{ request.url };
        //检查请求url是否在rpc函数映射表中
        if (rpc_table.find(rpc_url) != rpc_table.end()) {
            //如果存在，调用该rpc函数，传入请求，得到响应结果res
            auto res = rpc_table[rpc_url](request.content);
            m_keep_alive = request.keep_alive;
            //构建http响应头和响应体
            stringstream ss;
            ss << "HTTP/1.1 200 OK\r\n";
            ss << "Connection: " << (m_keep_alive ? "keep-alive" : "close") << "\r\n";
            ss << "Content-Length: " << res.size() << "\r\n\r\n";
            ss << res;
            m_response_buffer.s = ss.str();
            //打印请求信息
            //fmt::println(R"([{}][{}](RPC) method="{}", url1="{}", version="{}", host="{}", keep-alive={}, content-length={}, content={})", get_addr_str(), static_cast<short>(m_response_buffer.code), request.method, request.url, request.version, request.host, request.keep_alive, request.content_length, request.content);
            return IOState::OK;
            }
        }//constexpr

    //处理文件请求
    //构建请求文件的完整路径，如果请求的 URL 长度小于 2，添加默认的索引页名称
    auto file_name = string{html_dir} + string{request.url};
    if (request.url.size() < 2)
        file_name += default_index_page_name;

    struct stat file_state {};
    //lambda函数，根据文件状态设置响应状态码
    m_response_buffer.code = [&] {
        //文件不存在
        if (stat(file_name.data(), &file_state) != 0)
            return ResponseCode::NOT_FOUND;
        //请求的是目录，添加默认索引页名称，再次检查
#ifdef _WIN32
        if (file_state.st_mode& FILE_ATTRIBUTE_DIRECTORY) {
#else
        if (S_ISDIR(file_state.st_mode)) {
#endif
            file_name += "/";
            file_name += default_index_page_name;
            if (stat(file_name.data(), &file_state) != 0)
                return ResponseCode::BAD_REQUEST;
            }
#ifdef _WIN32
        bool is_readable = true;  // Windows 默认所有文件都可读
#else
        bool is_readable = (file_state.st_mode & S_IROTH);
#endif
        if (!is_readable) //文件对其他用户不可读
            return ResponseCode::FORBIDDEN;
        return ResponseCode::OK;
        }();

    //记录文件描述符和文件大小
    if (m_response_buffer.code == ResponseCode::OK) {
#ifdef _WIN32
        m_response_buffer.file_fd = _open(file_name.data(), _O_RDONLY | _O_BINARY);
        if (m_response_buffer.file_fd == -1) {
            // 错误处理
            }
#else
        m_response_buffer.file_fd = open(file_name.data(), O_RDONLY);
        if (m_response_buffer.file_fd == -1) {
            // 错误处理
            }
#endif
        m_response_buffer.file_fd = open(file_name.data(), O_RDONLY);
        m_response_buffer.file_size = file_state.st_size;
        m_keep_alive = request.keep_alive;
        }
    //打印请求信息，不打印为2293236 pages/min
    if constexpr (true)
        fmt::println(
            R"([{}][{}] method="{}", url2="{}", version="{}", host="{}", keep-alive={}, content-length={})",
            get_addr_str(), static_cast<short>(m_response_buffer.code),
            request.method, request.url, request.version, request.host,
            request.keep_alive, request.content_length);

    //构建响应
    stringstream ss;
    auto code = m_response_buffer.code;
    ss << "HTTP/1.1 " << static_cast<short>(code) << " " << http_code_to_title[code] << "\r\n";
    ss << "Connection: " << (m_keep_alive ? "keep-alive" : "close") << "\r\n";
    ss << "Content-Length: " << (code == ResponseCode::OK ? file_state.st_size : http_code_to_body.at(code).size()) << "\r\n";
    ss << "\r\n";
    if (code != ResponseCode::OK)//如果响应状态码不是OK，将错误信息添加到响应体
        ss << http_code_to_body.at(code);
    m_response_buffer.s = ss.str();
    return IOState::OK;
    }//work

auto Handler::write(int fd) -> IOState {
    m_last_alive_time = m_lazy_current_time;
    //  fmt::println("response is", m_response_buffer.s);
    while (m_response_buffer.write_index < m_response_buffer.s.size()) {
        auto sent_bytes = send(fd, m_response_buffer.s.data() + m_response_buffer.write_index, static_cast<int>(m_response_buffer.s.size() - m_response_buffer.write_index), 0);
        if (sent_bytes < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK)
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
                return IOState::PENDING;
            return IOState::BAD;
            }
        else if (sent_bytes == 0)
            return IOState::BAD;
        m_response_buffer.write_index += sent_bytes;
        }
    if (m_response_buffer.file_fd != -1) {
#ifdef _WIN32
        // Windows doesn't have sendfile, we need to implement a fallback
        // This is a simplified version - for production you'd want proper buffering
        const size_t chunk_size = 4096;
        vector<char> buffer(chunk_size);
        while (m_response_buffer.file_write_index < m_response_buffer.file_size) {
            // Seek to current position
            _lseek(m_response_buffer.file_fd, static_cast<long>(m_response_buffer.file_write_index), SEEK_SET);
            // Read chunk
            auto read_bytes = _read(m_response_buffer.file_fd, buffer.data(),
                                    static_cast<unsigned int>(min(chunk_size, m_response_buffer.file_size - m_response_buffer.file_write_index)));

            if (read_bytes <= 0) {
                break;
                }
            // Send chunk
            auto sent_bytes = ::send(fd, buffer.data(), read_bytes, 0);
            if (sent_bytes < 0) {
                if (WSAGetLastError() == WSAEWOULDBLOCK)
                    return IOState::PENDING;
                return IOState::BAD;
                }
            m_response_buffer.file_write_index += sent_bytes;
            }
#else
        while (m_response_buffer.file_write_index < m_response_buffer.file_size) {
            off_t off_write_index = static_cast<off_t>(m_response_buffer.file_write_index);
            auto sent_bytes = sendfile(fd, m_response_buffer.file_fd, &off_write_index,
                                       m_response_buffer.file_size - m_response_buffer.file_write_index);
            if (sent_bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return IOState::PENDING;
                return IOState::BAD;
                }
            else if (sent_bytes == 0)
                return IOState::BAD;
            }
#endif
        close(m_response_buffer.file_fd);
        }
    if (m_keep_alive) {
        clear();
        return IOState::KEEP_ALIVE;
        }
    return IOState::OK;
    }

auto Handler::clear() -> void {
    m_read_index = 0;
    m_request_parser = RequestParser();
    m_response_buffer = ResponseBuffer();
    }

Handler::Handler(chrono::steady_clock::time_point t,
                 const sockaddr_in &addr)
    : m_read_index{0}, m_request_parser(), m_response_buffer(),
      m_keep_alive{false}, m_last_alive_time{t}, m_lazy_current_time{t},
      m_addr{addr} {}
void Handler::update_current_time(chrono::steady_clock::time_point t) {
    m_lazy_current_time = t;
    }

} // namespace m::net::http
