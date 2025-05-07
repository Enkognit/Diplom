#ifndef ASYNC_FD_H
#define ASYNC_FD_H

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <variant>

#include "epoll_loop.h"
#include "io_result.h"
#include "logging.h"
#include "message.h"

#undef TAG
#define TAG "AsyncFd"

namespace diplom::transport::ip {

template <typename T> using ResolveFunction = std::function<void(const T &)>;

struct SendFunctor {
    ssize_t operator()(int fd, char *buf, size_t len)
    {
        return send(fd, buf, len, MSG_NOSIGNAL);
    }
};

struct RecvFunctor {
    ssize_t operator()(int fd, char *buf, size_t len)
    {
        return recv(fd, buf, len, 0);
    }
};

class AsyncFd final : public std::enable_shared_from_this<AsyncFd> {
    AsyncFd(int sock, const std::shared_ptr<EpollLoop> &loop) : fd_(sock), loop_(loop) {}

public:
    AsyncFd(const AsyncFd &) = delete;
    AsyncFd &operator=(const AsyncFd &) = delete;

    AsyncFd(AsyncFd &&rhs) noexcept : fd_(rhs.fd_), loop_(std::move(rhs.loop_))
    {
        rhs.fd_ = -1;
    }

    AsyncFd &operator=(AsyncFd &&rhs) noexcept
    {
        if (this != &rhs) {
            fd_ = rhs.fd_;
            loop_ = std::move(rhs.loop_);
            rhs.fd_ = -1;
        }
        return *this;
    }

    ~AsyncFd()
    {
        LOGT("Start");
        Release();
    }

    static std::shared_ptr<AsyncFd> Create(int fd, const std::shared_ptr<EpollLoop> &loop)
    {
        LOGT("Creating");
        return std::shared_ptr<AsyncFd>(new AsyncFd(fd, loop));
    }

    bool RecvMessage(const ResolveFunction<IoResult<Message>> &resolve)
    {
        if (token_) {
            LOGE("[%d] Operation for this object already registered.", fd_);
            return false;
        }

        LOGT("[%d] Receiving message", fd_);
        auto self = shared_from_this();

        auto msg_len_buf = std::make_shared<std::vector<char>>(sizeof(uint32_t));
        auto read_data = [this, msg_len_buf, resolve, self](IoResult<std::monostate> res_len) {
            if (res_len.IsErr()) {
                LOGE("RecvMessage: [%d] Failed to read message length", fd_);
                Deregister();
                resolve(IoResult<Message>::Err(res_len.GetErr().value()));
                return;
            }

            uint32_t msg_len;
            std::memcpy(&msg_len, msg_len_buf->data(), sizeof(msg_len));

            assert(msg_len > 0);

            LOGT("RecvMessage: [%d] Finished reading message len: %u", fd_, msg_len);

            auto data = std::make_shared<Message>(msg_len);
            auto cb = [this, resolve, data, self](IoResult<std::monostate> res_data) mutable {
                if (res_data.IsErr()) {
                    LOGE("RecvMessage: [%d] Failed to read message body", fd_);
                    Deregister();
                    resolve(IoResult<Message>::Err(res_data.GetErr().value()));
                    return;
                }

                LOGT("RecvMessage: [%d] Successfully read message, len=%zu", fd_, data->size());
                Deregister();
                resolve(IoResult<Message>::Ok(std::move(*data)));
            };

            ProcData(RecvFunctor(), reinterpret_cast<char *>(data->data()), msg_len, EPOLLIN, cb);
        };
        ProcData(RecvFunctor(), msg_len_buf->data(), sizeof(uint32_t), EPOLLIN, read_data);

        return true;
    }

    bool SendMessage(Message &&data, const ResolveFunction<IoResult<std::monostate>> &resolve)
    {
        if (token_) {
            LOGE("[%d] Operation for this object already registered.", fd_);
            return false;
        }

        uint32_t msg_len = data.size();
        assert(msg_len > 0);
        LOGT("[%d] Sending message, len=%u", fd_, msg_len);

        auto self = shared_from_this();
        auto sdata = std::make_shared<Message>(std::move(data));

        auto msg_len_buf = std::make_shared<std::vector<char>>(sizeof(msg_len));
        std::memcpy(msg_len_buf->data(), &msg_len, sizeof(msg_len));

        // called after sending length of Message
        auto send_data = [this, self, msg_len_buf, sdata, resolve](IoResult<std::monostate> res_len) {
            if (res_len.IsErr()) {
                LOGE("SendMessage: [%d] Failed to send message length", fd_);
                Deregister();
                resolve(res_len);
                return;
            }

            LOGT("SendMessage: [%d] Finished sending message len: %zu bytes", fd_, sizeof(uint32_t));

            // called after sending data part of Message
            auto cb = [this, resolve, self, sdata](IoResult<std::monostate> res_data) {
                if (res_data.IsErr()) {
                    LOGE("SendMessage: [%d] Failed to send message body", fd_);
                    Deregister();
                    resolve(res_data);
                    return;
                }

                LOGT("SendMessage: [%d] Successfully sent message", fd_);
                Deregister();
                resolve(res_data);
            };
            ProcData(SendFunctor(), reinterpret_cast<char *>(sdata->data()), sdata->size(), EPOLLOUT, cb);
        };
        ProcData(SendFunctor(), msg_len_buf->data(), sizeof(uint32_t), EPOLLOUT, send_data);

        return true;
    }

    int Get() const
    {
        return fd_;
    }

    int Release()
    {
        LOGT("[%d] Releasing socket", fd_);
        if (token_) {
            Deregister();
        }
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    void Deregister()
    {
        if (!token_) {
            LOGE("[%d] No registered operation", fd_);
            return;
        }

        LOGT("[%d] Deregistering", fd_);
        loop_->DeregisterAction(token_);
        token_ = 0;
    }

    template <class Func>
    void ProcData(Func func, char *data, size_t size, uint32_t events, //
                  const ResolveFunction<IoResult<std::monostate>> &resolve)
    {
        LOGT("Start");
        auto self = shared_from_this();
        size_t left_size = size;
        auto cb = [this, func, left_size, size, resolve, data, self](EpollResult res) mutable {
            LOGT("ProcData callback: Start");
            LOGT("[%d] ProcData: left_size=%zu", fd_, left_size);
            if (res.IsErr()) {
                switch (res.GetErr().value()) {
                    case IoError::Timeout:
                        LOGT("[%d] Timeout triggered before write operation finished", fd_);
                    case IoError::Error:
                        LOGE("[%d] Error occured on socket", fd_);
                }
                resolve(res);
                return;
            }

            LOGT("ProcData: Before calling function");
            ssize_t length = func(fd_, data + (size - left_size), left_size);
            LOGT("ProcData: After calling function: %zi %s", length, strerror(errno));
            if constexpr (std::is_same<Func, RecvFunctor>::value) {
                if ((length < 0 && errno != EAGAIN) || (length == 0 && left_size != 0)) {
                    LOGE("Failed to process full data size [%zu/%zu]: %s", size - left_size, size, strerror(errno));
                    resolve(IoResult<std::monostate>::Err(IoError::Error));
                    return;
                }
            } else {
                if (length < 0) {
                    LOGE("Failed to process full data size [%zu/%zu]: %s", size - left_size, size, strerror(errno));
                    resolve(IoResult<std::monostate>::Err(IoError::Error));
                    return;
                }
            }

            left_size -= length;
            if (left_size == 0) {
                LOGT("[%d] Data processed fully: %zu bytes", fd_, size);
                resolve(IoResult<std::monostate>::Ok(std::monostate{}));
                return;
            }
            LOGT("[%d] Data not fully processed [%zu/%zu]", fd_, size - left_size, size);
        };

        if (!token_) {
            auto dir = IoDirection::INPUT;
            if constexpr (std::is_same<Func, SendFunctor>::value) {
                dir = IoDirection::OUTPUT;
            }
            LOGT("Registering new action for %s", ToString(dir));
            if (!loop_->RegisterAction(fd_, dir, cb, token_, self)) {
                LOGE("[%d] Failed to register action", fd_);
                resolve(IoResult<std::monostate>::Err(IoError::Error));
                return;
            }
            LOGT("[%d] Got token: %u", fd_, token_);
        } else {
            LOGT("Replacing aciton");
            if (!loop_->ReplaceAction(token_, cb)) {
                LOGE("[%d] Failed to replace action", fd_);
                resolve(IoResult<std::monostate>::Err(IoError::Error));
                return;
            }
        }
    }

    int fd_ = -1;
    std::shared_ptr<EpollLoop> loop_;
    CancellationToken token_ = 0;
};

} 

#endif
