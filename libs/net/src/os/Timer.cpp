//
// Created by wwww on 2023/8/25.
//
#include <os.h>
#include <stdexcept>
#include <sys/timerfd.h>
using namespace std;
namespace m::os {

Timer::Timer(size_t seconds, size_t nanoseconds) {
    struct itimerspec timer_opt {};
    m_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (m_fd < 0)
        throw runtime_error{"cannot create timer"};

    timer_opt.it_value.tv_sec = seconds;
    timer_opt.it_value.tv_nsec = nanoseconds;
    timer_opt.it_interval.tv_sec = seconds;
    timer_opt.it_interval.tv_nsec = nanoseconds;

    if (timerfd_settime(m_fd, 0, &timer_opt, nullptr) < 0)
        throw runtime_error{"cannot create timer"};
    }

uint64_t Timer::tick() const {
    uint64_t count = 0;
    ssize_t bytes_read = read(m_fd, (void *)&count, sizeof(count));
    if (bytes_read == -1) {
        throw runtime_error("Failed to read from timer");
    }
    else if (bytes_read != sizeof(count)) {
        throw runtime_error("Incomplete read from timer");
    }
    return count;
    }
} // namespace m::os