//
// Created by wwww on 2023/8/25.
//
#include <fmt/core.h>
#include <iomanip>
#include <protocol/http.h>
namespace m::net::http {
auto RequestParser::next_line(std::string_view text)
-> std::pair<IOState, std::string_view> {
    // fmt::println("{}", text);
    auto len = text.size();
    for (auto i = m_pivot; i < len; ++i) {
        auto c = text[i];
        if (c == '\r') {
            if (i + 1 == len)
                return {IOState::PENDING, {}};
            else if (text[i + 1] == '\n') {
                std::string_view line{text.data() + m_pivot, i - m_pivot};
                //                fmt::println(R"(i={}, pivot={}, line="{}")", i,
                //                m_pivot, line);
                m_pivot = i + 2;
                return {IOState::OK, line};
                }
            return {IOState::BAD, {}};
            }
        }
    return {IOState::PENDING, {}};
    }

auto RequestParser::parse_content(std::string_view text) -> IOState {
    if (m_request.content_length + m_pivot < text.size())
        return IOState::PENDING;
    m_request.content = std::string_view {text.data() + m_pivot, m_request.content_length};
    return IOState::OK;
    }

auto RequestParser::parse_header_line(std::string_view text) -> IOState {
    constexpr static std::string_view CONNECTION_PREFIX = "Connection",
    CONTENT_LENGTH_PREFIX = "Content-Length",
    HOST_PREFIX = "Host";
    auto [state, line] = next_line(text);
    //  fmt::println("header: \"{}\", state = {}", line,
    //  static_cast<size_t>(state));
    if (state != IOState::OK)
        return state;

    // end of headers
    if (line.empty()) {
        m_protocol_state = ProtocolState::ON_CONTENT;
        return IOState::OK;
        }

    auto sep = line.find_first_of(':');
    if (sep == decltype(line)::npos)
        return IOState::BAD;
    auto start_of_value = line.find_first_not_of(" \t", sep + 1);
    if (start_of_value == decltype(line)::npos)
        return IOState::BAD;
    auto field = std::string_view{line.data(), sep};
    auto value = std::string_view{line.data() + start_of_value,
                                  line.size() - start_of_value};
    if (CONNECTION_PREFIX == field) {
        m_request.keep_alive = (value == "keep-alive");
        //    fmt::println("line=<{}>, value=<{}>", line, value);
        }
    else if (CONTENT_LENGTH_PREFIX == field)
        m_request.content_length = std::atoi(value.data());
    else if (HOST_PREFIX == field)
        m_request.host = value;
    else
        ;
    //    fmt::println("[HEADER] skipped: {}", field);
    return IOState::OK;
    }

auto RequestParser::parse_request_line(std::string_view text) -> IOState {
    auto [state, line] = next_line(text);
    //  fmt::println(R"([HTTP] got line: "{}", state={})", line,
    //  static_cast<size_t>(state));
    if (state != IOState::OK)
        return state;
    auto end_of_method = line.find_first_of(" \t");
    if (end_of_method == decltype(line)::npos)
        return IOState::BAD;
    auto end_of_url = text.find_first_of(" \t", end_of_method + 1);
    if (end_of_url == decltype(line)::npos)
        return IOState::BAD;
    m_request.method = std::string_view{line.data(), end_of_method};
    m_request.url = std::string_view{line.data() + end_of_method + 1,
                                     end_of_url - end_of_method - 1};
    m_request.version = std::string_view{line.data() + end_of_url + 1,
                                         line.size() - end_of_url - 1};
    m_protocol_state = ProtocolState::ON_HEADERS;
    return IOState::OK;
    }

auto RequestParser::update(std::string_view text) -> IOState {
    //  fmt::println("got {}", text);
    while (true) {
        switch (m_protocol_state) {
            case ProtocolState::ON_REQUEST_LINE: {
                auto state = parse_request_line(text);
                if (state != IOState::OK)
                    return state;
                break;
                }
            case ProtocolState::ON_HEADERS: {
                auto state = parse_header_line(text);
                if (state != IOState::OK)
                    return state;
                break;
                }
            case ProtocolState::ON_CONTENT: {
                return parse_content(text);
                }
            }
        }
    return IOState::PENDING;
    }

} // namespace m::net::http