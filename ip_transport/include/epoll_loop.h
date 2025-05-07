#ifndef EPOLL_LOOP_H
#define EPOLL_LOOP_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <sys/epoll.h>
#include <unordered_map>

#include "io_result.h"

namespace diplom::transport::ip {

using EpollResult = IoResult<std::monostate>;
using DescriptorCallback = std::function<void(EpollResult)>;

class AsyncFd;

enum class IoDirection {
    INPUT = EPOLLIN,
    OUTPUT = EPOLLOUT,

};

static IoDirection GetOpposite(IoDirection dir)
{
    switch (dir) {
        case IoDirection::INPUT:
            return IoDirection::OUTPUT;
        case IoDirection::OUTPUT:
            return IoDirection::INPUT;
        default:
            return dir;
    }
}

using CancellationToken = uint64_t;

static constexpr char const *ToString(IoDirection dir)
{
    switch (dir) {
        case IoDirection::INPUT:
            return "INPUT";
        case IoDirection::OUTPUT:
            return "OUTPUT";
    }
    return "unknown";
}

struct FdDirection {
    int fd = -1;
    uint32_t direction = 0;

    FdDirection(int fd, uint32_t dir) : fd(fd), direction(dir) {}

    bool operator==(const FdDirection &other) const
    {
        return fd == other.fd && direction == other.direction;
    }
};

struct CallbackKeeper {
    std::unique_ptr<DescriptorCallback> callback = nullptr;
    std::shared_ptr<AsyncFd> keepLink = nullptr;
};
}

template <> struct std::hash<diplom::transport::ip::FdDirection> {
    size_t operator()(const diplom::transport::ip::FdDirection &key) const
    {
        return std::hash<int>()(key.fd) ^ (std::hash<uint32_t>()(key.direction) << 1);
    }
};

namespace diplom::transport::ip {
class EpollLoop final {
public:
    EpollLoop() = default;
    EpollLoop(const EpollLoop &loop) = delete;
    EpollLoop &operator=(const EpollLoop &loop) = delete;
    ~EpollLoop();

    bool Init();
    bool RegisterAction(int fd, IoDirection dir, const DescriptorCallback &callback, CancellationToken &token,
                        std::shared_ptr<AsyncFd> afd = nullptr, int timeoutMs = 0);
    bool ReplaceAction(CancellationToken, const DescriptorCallback &callback);
    bool DeregisterAction(CancellationToken token);
    bool DeregisterFd(int fd);
    void Run();
    void Stop() const;

private:
    int EpollCtl(int fd, int cmd, uint32_t ev = 0) const;
    void UtilizeCallback(std::unique_ptr<DescriptorCallback> callback);
    void RunCallback(int fd, uint32_t ev, EpollResult res);

    int epollHandle_ = -1;
    int removeEvent_ = -1;
    int stopEvent_ = -1;

    std::mutex globalLock_;
    std::unordered_map<FdDirection, CallbackKeeper> fdToCallback_;
    std::unordered_map<CancellationToken, std::pair<int, IoDirection>> cancelMap;

    std::unordered_map<int, int> fdToTimer_;
    std::unordered_map<int, int> timerToFd_;
    std::queue<std::unique_ptr<DescriptorCallback>> removeQueue_;
    CancellationToken nextToken_ = 1;
};

} 

#endif
