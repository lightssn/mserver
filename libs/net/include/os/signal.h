//
// Created by wwww on 2023/8/23.
//

#ifndef CPP_SIMPLE_WEB_SERVER_SIGNAL_HPP
#define CPP_SIMPLE_WEB_SERVER_SIGNAL_HPP
namespace m::os {
void handle_signal(int sig, void (*handler)(int), bool restart = true);
}
#endif // CPP_SIMPLE_WEB_SERVER_SIGNAL_HPP
