#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <utility>

#include "epoll_loop.h"
#include "ip_utils.h"
#include "logging.h"

#undef TAG
#define TAG "EpollLoop"

namespace diplom::transport::ip {

static constexpr size_t MAX_EPOLL_EVENTS = 32;
static constexpr size_t EPOLL_TIMEOUT = 1000;
static constexpr long MILLIS_IN_SEC = 1000;
static constexpr long NANOS_IN_MILLI = 1000 * 1000;

bool EpollLoop::Init()
{
    epollHandle_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollHandle_ == -1) {
        LOGE("Failed to create epoll handle: %s", strerror(errno));
        return false;
    }

    removeEvent_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (removeEvent_ == -1) {
        LOGE("Failed to create remove eventfd handle: %s", strerror(errno));
        return false;
    }

    stopEvent_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stopEvent_ == -1) {
        LOGE("Failed to create stop eventfd handle: %s", strerror(errno));
        return false;
    }

    if (EpollCtl(removeEvent_, EPOLL_CTL_ADD, EPOLLIN) == -1) {
        LOGE("Failed to add remove eventfd to epoll: %s", strerror(errno));
        return false;
    }

    if (EpollCtl(stopEvent_, EPOLL_CTL_ADD, EPOLLIN) == -1) {
        LOGE("Failed to add stop eventfd to epoll: %s", strerror(errno));
        return false;
    }

    LOGT("Initialized successfully");
    return true;
}

void EpollLoop::RunCallback(int fd, uint32_t ev, EpollResult res)
{
    LOGT("Start running callback");
    LOGT("Aquire globalLock_");
    std::unique_lock guard(globalLock_);
    LOGT("got globalLock_");
    auto find = fdToCallback_.find(FdDirection(fd, ev));
    if (find != fdToCallback_.end()) {
        LOGT("Found callback %p", find->second.callback.get());
        auto cb = find->second.callback.get();
        // callback logic may access epoll, so call it unlocked
        LOGT("Release globalLock_");
        guard.unlock();
        LOGT("Released lock, now run callback: fd=%d, ev=%s", fd, EpollEventToString(ev).c_str());
        (*cb)(res);
    } else {
        LOGT("Callback not found");
    }
}

void EpollLoop::Run()
{
    while (true) {
        LOGT("entering epoll to get some events");
        epoll_event events[MAX_EPOLL_EVENTS];
        int events_to_handle = epoll_wait(epollHandle_, events, MAX_EPOLL_EVENTS, EPOLL_TIMEOUT);
        if (events_to_handle < 0) {
            LOGE("epoll_wait returned error: %s", strerror(errno));
            return;
        }

        LOGT("epoll got some events");

        for (int idx = 0; idx < events_to_handle; idx++) {
            int fd = events[idx].data.fd;
            if (fd == stopEvent_) {
                uint64_t u;
                read(stopEvent_, &u, sizeof(u));
                return;
            } else if (fd == removeEvent_) {
                uint64_t u;
                read(removeEvent_, &u, sizeof(u));
                LOGT("Aquire globalLock_");
                std::unique_lock guard(globalLock_);
                LOGT("got globalLock_");
                while (!removeQueue_.empty()) {
                    {
                        LOGT("Utilizing callback");
                        auto c = std::move(removeQueue_.front());
                        removeQueue_.pop();
                        LOGT("Release globalLock_");
                        guard.unlock();
                    }
                    LOGT("Aquire globalLock_");
                    guard.lock();
                    LOGT("got globalLock_");
                }
                LOGT("Release globalLock_");
            } else {
                uint32_t ev = events[idx].events;

                if ((ev & EPOLLHUP) || (ev & EPOLLERR)) {
                    LOGT("ERROR event on fd=%d", fd);
                    RunCallback(fd, EPOLLIN, EpollResult::Err(IoError::Error));
                    RunCallback(fd, EPOLLOUT, EpollResult::Err(IoError::Error));
                } else if (ev & EPOLLIN) {
                    LOGT("EPOLLIN event on fd=%d", fd);
                    RunCallback(fd, EPOLLIN, EpollResult::Ok(std::monostate{}));
                } else if (ev & EPOLLOUT) {
                    LOGT("EPOLLOUT event on fd=%d", fd);
                    RunCallback(fd, EPOLLOUT, EpollResult::Ok(std::monostate{}));
                }
            }
        }
    }
}

bool EpollLoop::RegisterAction(int fd, IoDirection dir, const DescriptorCallback &callback, CancellationToken &token,
                               std::shared_ptr<AsyncFd> afd, int timeoutMs)
{
    assert(token == 0);

    LOGT("Aquire globalLock_");
    std::lock_guard guard(globalLock_);
    LOGT("got globalLock_");
    LOGT("Register action for fd=%d direction %s", fd, ToString(dir));

    auto keeper = fdToCallback_.find(FdDirection(fd, (uint32_t)dir));

    if (keeper != fdToCallback_.end()) {
        LOGE("Action on fd=%d for direction %s already registered", fd, ToString(dir));
        return false;
    }

    uint32_t events = 0;
    int cmd = 0;

    if (fdToCallback_.find(FdDirection(fd, (uint32_t)GetOpposite(dir))) != fdToCallback_.end()) {
        cmd = EPOLL_CTL_MOD;
        events |= (uint32_t)IoDirection::INPUT;
        events |= (uint32_t)IoDirection::OUTPUT;
    } else {
        cmd = EPOLL_CTL_ADD;
        events = (uint32_t)dir;
    }

    if (EpollCtl(fd, cmd, events | EPOLLHUP | EPOLLERR) == -1) {
        LOGE("Failed to communicate with epoll: %s", strerror(errno));
        return false;
    }

    LOGT("Adding action");

    fdToCallback_.emplace(FdDirection(fd, (uint32_t)dir),
                          CallbackKeeper{std::make_unique<DescriptorCallback>(callback), std::move(afd)});
    cancelMap.emplace(nextToken_, std::make_pair(fd, dir));

    token = nextToken_++;
    LOGT("Release globalLock_");
    return true;
}

bool EpollLoop::ReplaceAction(CancellationToken token, const DescriptorCallback &callback)
{
    assert(token > 0);

    LOGT("Aquire globalLock_");
    std::lock_guard guard(globalLock_);
    LOGT("got globalLock_");
    LOGT("Replacing the action");
    auto find_token = cancelMap.find(token);
    if (find_token == cancelMap.end()) {
        LOGE("Cannot deregister action. Token not found: %d", token);
        return false;
    }
    auto [fd, dir] = find_token->second;

    auto keeper = fdToCallback_.find(FdDirection(fd, (uint32_t)dir));
    if (keeper == fdToCallback_.end()) {
        LOGF("Cannot deregister action. Action not found for this direction: fd=%d, dir=%s", fd, ToString(dir));
        return false;
    }

    UtilizeCallback(std::move(keeper->second.callback));
    keeper->second.callback = std::make_unique<DescriptorCallback>(callback);

    LOGT("Done");
    LOGT("Release globalLock_");
    return true;
}

void EpollLoop::UtilizeCallback(std::unique_ptr<DescriptorCallback> callback)
{
    removeQueue_.push(std::move(callback));
    uint64_t u = 1;
    write(removeEvent_, &u, sizeof(u));
}

bool EpollLoop::DeregisterAction(CancellationToken token)
{
    LOGT("Aquire globalLock_");
    std::lock_guard guard(globalLock_);
    LOGT("got globalLock_");
    LOGT("Deregistering action");

    auto find_token = cancelMap.find(token);
    if (find_token == cancelMap.end()) {
        LOGE("Cannot deregister action. Token not found: %d", token);
        return false;
    }
    auto [fd, dir] = find_token->second;

    auto keeper = fdToCallback_.find(FdDirection(fd, (uint32_t)dir));
    if (keeper == fdToCallback_.end()) {
        LOGD("Cannot deregister action. Action not found for this direction: fd=%d, dir=%s", fd, ToString(dir));
        cancelMap.erase(token);
        return false;
    }

    UtilizeCallback(std::move(keeper->second.callback));
    fdToCallback_.erase(FdDirection(fd, (uint32_t)dir));
    cancelMap.erase(token);

    uint32_t events = 0;
    int cmd = 0;

    if (fdToCallback_.find(FdDirection(fd, (uint32_t)GetOpposite(dir))) != fdToCallback_.end()) {
        cmd = EPOLL_CTL_MOD;
        events |= (uint32_t)GetOpposite(dir);
    } else {
        cmd = EPOLL_CTL_DEL;
        events = (uint32_t)dir;
    }

    if (EpollCtl(fd, cmd, events) == -1) {
        LOGF("Failed to communicate with epoll: %s", strerror(errno));
        return false;
    }
    LOGT("Release globalLock_");
    return true;
}

bool EpollLoop::DeregisterFd(int fd)
{
    LOGT("Aquire globalLock_");
    std::lock_guard guard(globalLock_);
    LOGT("got globalLock_");
    LOGT("Deregistering socket: fd=%d", fd);

    bool got = false;
    auto keeper = fdToCallback_.find(FdDirection(fd, EPOLLIN));
    if (keeper != fdToCallback_.end()) {
        auto cb = std::move(keeper->second.callback);
        fdToCallback_.erase(FdDirection(fd, EPOLLIN));
        UtilizeCallback(std::move(cb));
        got = true;
    }

    keeper = fdToCallback_.find(FdDirection(fd, EPOLLOUT));
    if (keeper != fdToCallback_.end()) {
        auto cb = std::move(keeper->second.callback);
        fdToCallback_.erase(FdDirection(fd, EPOLLOUT));
        UtilizeCallback(std::move(cb));
        got = true;
    }

    if (got) {
        EpollCtl(fd, EPOLL_CTL_DEL);
    }
    LOGT("Release globalLock_");
    return true;
}

int EpollLoop::EpollCtl(int fd, int cmd, uint32_t ev) const
{
    LOGT("fd=%d cmd=%d ev=%d", fd, cmd, ev);
    if (ev) {
        epoll_event event{};
        event.events = ev;
        event.data.fd = fd;
        return epoll_ctl(epollHandle_, cmd, fd, &event);
    } else {
        return epoll_ctl(epollHandle_, cmd, fd, nullptr);
    }
}

void EpollLoop::Stop() const
{
    uint64_t u = 1;
    write(stopEvent_, &u, sizeof(u));
}

EpollLoop::~EpollLoop()
{
    Stop();
    close(epollHandle_);
    close(removeEvent_);
    close(stopEvent_);
}

} 
