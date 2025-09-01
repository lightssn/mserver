#include <os.h>
#include <stdexcept>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/timerfd.h>
#include <unistd.h>
#endif

namespace m::os {

Timer::Timer(size_t seconds, size_t nanoseconds) {
#ifdef _WIN32
    // Windows 实现
    //m_timer_handle = CreateWaitableTimer(NULL, TRUE, NULL);
    //if (m_timer_handle == NULL) {
    //    throw std::runtime_error("Cannot create timer");
    //    }

    //LARGE_INTEGER due_time;
    //due_time.QuadPart = -static_cast<LONGLONG>(seconds * 10000000 + nanoseconds / 100);

    //if (!SetWaitableTimer(
    //            m_timer_handle,
    //            &due_time,
    //            static_cast<LONG>(seconds * 1000 + nanoseconds / 1000000),
    //            NULL, NULL, FALSE)) {
    //    throw std::runtime_error("Cannot set timer");
    //    }
#else
    // Linux 实现
    struct itimerspec timer_opt {};
    m_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (m_fd < 0) {
        throw std::runtime_error("Cannot create timer");
        }

    timer_opt.it_value.tv_sec = seconds;
    timer_opt.it_value.tv_nsec = nanoseconds;
    timer_opt.it_interval.tv_sec = seconds;
    timer_opt.it_interval.tv_nsec = nanoseconds;

    if (timerfd_settime(m_fd, 0, &timer_opt, nullptr) < 0) {
        throw std::runtime_error("Cannot set timer");
        }
#endif
    }

} // namespace m::os