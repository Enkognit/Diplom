#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "async_fd.h"
#include "epoll_loop.h"
#include "error.h"
#include "io_result.h"
#include "ip_conn_manager.h"
#include "ip_transport.h"
#include "ip_utils.h"
#include "logging.h"

#undef TAG
#define TAG "ConnectionManager"

namespace diplom::transport::ip
{

    int ConnectionManager::Init(IpTransport *ipTransport, std::shared_ptr<EpollLoop> &loop)
    {
        if (!ipTransport)
        {
            LOGE("IpTransport is null, cannot init");
            return -1;
        }

        ipTransport_ = ipTransport;
        loop_ = loop;

        return 0;
    }

    bool ConnectionManager::Start()
    {
        if ((listenFd_ = common::create_tcp_socket(TCP_LISTEN_PORT)) == -1)
        {
            LOGE("Failed to create listen socket: %s", strerror(errno));
            return false;
        }
        if (listen(listenFd_, 1000))
        {
            LOGE("Failed to listen on socket: %s", strerror(errno));
            close(listenFd_);
            return false;
        }

        auto cb = [this](EpollResult res)
        { AcceptConn(); };

        CancellationToken listen_token = 0;
        if (!loop_->RegisterAction(listenFd_, IoDirection::INPUT, cb, listen_token))
        {
            LOGE("Failed to register action");
            close(listenFd_);
            return false;
        }
        return true;
    }

    void ConnectionManager::AcceptConn()
    {
        LOGD("Start accepting incoming connection");
        auto [addr, client_fd] = common::accept_client(listenFd_);
        if (client_fd == -1)
        {
            LOGE("Failed to accept connection");
            return;
        }

        LOGD("Connection accepted.");

        IncomingHelloExchange(client_fd);
    }

    void ConnectionManager::IncomingHelloExchange(int fd)
    {
        LOGD("Start incoming hello exchange procedure");
        auto async_fd = AsyncFd::Create(fd, loop_);

        auto cb = [this, async_fd](IoResult<Message> res)
        {
            if (res.IsErr())
            {
                LOGE("Failed to receive hello message from peer");
                close(async_fd->Release());
                return;
            }

            Message msg = res.GetOk().value();
            auto o_hello = HelloMsg::Deserialize(msg);

            if (!o_hello)
            {
                LOGE("Received corrupted hello message");
                close(async_fd->Release());
                return;
            }

            HelloMsg hello = o_hello.value();
            if (hello.type != HelloType::Notify)
            {
                LOGE("Invalid hello message type. Expected %d, got %d", HelloType::Notify, hello.type);
                close(async_fd->Release());
                return;
            }

            auto oaddr = common::get_fd_addr(async_fd->Get());
            if (!oaddr)
            {
                LOGD("Cannot get neighbor address.");
                close(async_fd->Release());
                return;
            }
            auto peer = hello.id;

            LOGD("Received Hello::Notify from peer %s on address %s", PeerIdToString(peer).c_str(),
                 common::in_addr_to_string(oaddr.value().sin_addr).c_str());

            ipTransport_->brManager.ProcessHello(oaddr.value().sin_addr, hello, false);

            LOGT("Sending hello answer");

            auto hello_answer = HelloMsg{
                .id = ipTransport_->_self,
                .type = HelloType::Answer,
            };

            msg = hello_answer.Serialize();

            auto cb = [this, async_fd, peer](IoResult<std::monostate> res)
            {
                if (res.IsErr())
                {
                    LOGE("Failed to send hello answer");
                    close(async_fd->Release());
                    return;
                }

                LOGD("Incoming connection established with peer %s, fd = %d", PeerIdToString(peer).c_str(),
                     async_fd->Get());

                {
                    LOGT("get activeConnsMut_");
                    std::lock_guard guard(activeConnsMut_);
                    activeConns.insert({async_fd->Get(), {peer, LinkDirection::INCOMING}});
                    LOGT("release activeConnsMut_");
                }
                {
                    LOGT("get connToFdMut_");
                    std::lock_guard guard(connToFdMut_);
                    connToFd.insert({{peer, LinkDirection::INCOMING}, async_fd->Get()});
                    LOGT("release connToFdMut_");
                }
                {
                    LOGT("get sendQueueMut_");
                    std::lock_guard guard(sendQueueMut_);
                    sendQueues_.insert({async_fd->Get(), std::deque<Message>()});
                    LOGT("release sendQueueMut_");
                }

                ipTransport_->OnLinkEstablishing(peer, LinkDirection::INCOMING);

                ReadMessage(async_fd->Release());
            };

            async_fd->SendMessage(std::move(msg), cb, false);
        };
        async_fd->RecvMessage(cb);
    }

    void ConnectionManager::OutgoingHelloExchange(int fd, PeerId peer)
    {
        LOGD("Start outgoing hello exchange procedure");
        auto async_fd = AsyncFd::Create(fd, loop_);

        auto hello = HelloMsg{
            .id = ipTransport_->_self,
            .type = HelloType::Notify,
        };
        Message msg = hello.Serialize();

        auto cb = [this, async_fd, peer](IoResult<std::monostate> res)
        {
            if (res.IsErr())
            {
                LOGE("Failed to send hello notify");
                ipTransport_->OnLinkEstablishingError(peer, LinkDirection::OUTGOING);
                close(async_fd->Release());
                return;
            }

            LOGD("Hello::Notify sent successfully to peer %s", PeerIdToString(peer).c_str());

            auto cb = [this, async_fd, peer](IoResult<Message> res)
            {
                if (res.IsErr())
                {
                    LOGE("Failed to receive hello answer");
                    close(async_fd->Release());
                    return;
                }

                Message msg = res.GetOk().value();
                auto o_hello = HelloMsg::Deserialize(msg);

                if (!o_hello)
                {
                    LOGE("Received corrupted hello message");
                    close(async_fd->Release());
                    return;
                }

                HelloMsg hello = o_hello.value();
                if (hello.id != peer || hello.type != HelloType::Answer)
                {
                    LOGE("Received wrong hello message");
                    close(async_fd->Release());
                    return;
                }

                LOGD("Outgoing connection established with peer %s, fd = %d", PeerIdToString(peer).c_str(),
                     async_fd->Get());
                {
                    LOGT("get activeConnsMut_");
                    std::lock_guard guard(activeConnsMut_);
                    activeConns.insert({async_fd->Get(), {peer, LinkDirection::OUTGOING}});
                    LOGT("release activeConnsMut_");
                }
                {
                    LOGT("get connToFdMut_");
                    std::lock_guard guard(connToFdMut_);
                    connToFd.insert({{peer, LinkDirection::OUTGOING}, async_fd->Get()});
                    LOGT("release connToFdMut_");
                }
                {
                    LOGT("get sendQueueMut_");
                    std::lock_guard guard(sendQueueMut_);
                    sendQueues_.insert({async_fd->Get(), std::deque<Message>()});
                    LOGT("release sendQueueMut_");
                }

                ReadMessage(async_fd->Release());

                ipTransport_->OnLinkEstablishing(peer, LinkDirection::OUTGOING);
            };
            async_fd->RecvMessage(cb);
        };
        async_fd->SendMessage(std::move(msg), cb, false);
    }

    void ConnectionManager::EstablishConnection(PeerId peer)
    {
        LOGD("Start connection to %s", PeerIdToString(peer).c_str());

        {
            LOGT("get connToFdMut_");
            std::unique_lock guard(connToFdMut_);
            if (connToFd.count({peer, LinkDirection::OUTGOING}))
            {
                guard.unlock();
                LOGT("release connToFdMut_");
                LOGE("Outgoing session to this peer already exists");
                ipTransport_->OnLinkEstablishingError(peer, LinkDirection::OUTGOING, OpenSessionErrorKind::SESSION_EXISTS);
                return;
            }
            guard.unlock();
            LOGT("release connToFdMut_");
        }

        auto oaddr = ipTransport_->GetAddress(peer);
        if (!oaddr)
        {
            LOGE("Cannot identify IP address for peer");
            ipTransport_->OnLinkEstablishingError(peer, LinkDirection::OUTGOING, OpenSessionErrorKind::NO_SUCH_PEER);
            return;
        }

        LOGT("Start creating the socket");

        auto addr = oaddr.value();
        int fd = common::create_tcp_socket();
        if (fd == -1)
        {
            LOGE("Failed to create socket");
            ipTransport_->OnLinkEstablishingError(peer, LinkDirection::OUTGOING);
            return;
        }

        struct sockaddr_in endpoint = {
            .sin_family = AF_INET,
            .sin_port = htons(TCP_LISTEN_PORT),
            .sin_addr = addr,
        };

        LOGD("Connecting to %s", common::sockaddr_in_to_string(endpoint).c_str());

        int res = connect(fd, (sockaddr *)&endpoint, sizeof(endpoint));

        LOGT("Result of connect: %d, errno=%s", res, strerror(errno));

        if (res < 0 && errno != EINPROGRESS)
        {
            LOGE("Failed to establish connection.");
            ipTransport_->OnLinkEstablishingError(peer, LinkDirection::OUTGOING);
        }

        OutgoingHelloExchange(fd, peer);
    }

    void ConnectionManager::CloseConn(PeerId peer_id, LinkDirection dir)
    {
        LOGD("Start closing connection to %s", PeerIdToString(peer_id).c_str());

        {
            LOGT("get connToFdMut_");
            std::unique_lock guard(connToFdMut_);
            int fd = -1;
            if (dir == LinkDirection::ALL || dir == LinkDirection::INCOMING)
            {
                if (connToFd.count(make_pair(peer_id, LinkDirection::INCOMING)) > 0)
                {
                    fd = connToFd[make_pair(peer_id, LinkDirection::INCOMING)];
                    guard.unlock();
                    LOGT("release connToFdMut_");
                    CloseFD(fd);
                }
            }
        }

        {
            LOGT("get connToFdMut_");
            std::unique_lock guard(connToFdMut_);
            int fd = -1;
            if (dir == LinkDirection::ALL || dir == LinkDirection::OUTGOING)
            {
                if (connToFd.count(make_pair(peer_id, LinkDirection::OUTGOING)) > 0)
                {
                    fd = connToFd[make_pair(peer_id, LinkDirection::OUTGOING)];
                    guard.unlock();
                    LOGT("release connToFdMut_");
                    CloseFD(fd);
                }
            }
        }
    }

    void ConnectionManager::SendMessageFd(int fd)
    {
        LOGD("Start sending to FD");
        auto async_fd = AsyncFd::Create(fd, loop_);

        auto cb = [this, async_fd](IoResult<std::monostate> res)
        {
            {
                LOGT("get activeConnsMut_");
                std::lock_guard guard(activeConnsMut_);
                auto find = activeConns.find(async_fd->Get());
                if (find == activeConns.end())
                {
                    LOGD("Connection is not active already. Finish sending to fd=%d.", async_fd->Get());
                    LOGT("release activeConnsMut_");
                    return;
                }
                auto [peer, _] = find->second;

                if (res.IsErr())
                {
                    LOGE("Failed to send message to peer %s", PeerIdToString(peer).c_str());
                }
                else
                {
                    LOGD("Successfully sent message to peer %s", PeerIdToString(peer).c_str());
                }
                LOGT("release activeConnsMut_");
            }

            {
                LOGT("get sendQueueMut_");
                std::unique_lock guard(sendQueueMut_);
                if (sendQueues_.find(async_fd->Get()) == sendQueues_.end())
                {
                    LOGD("Send queues for fd=%d is absent. Finish sending.", async_fd->Get());
                    return;
                }

                sendQueues_[async_fd->Get()].pop_front();
                if (!sendQueues_[async_fd->Get()].empty())
                {
                    guard.unlock();
                    LOGT("release sendQueueMut_");
                    SendMessageFd(async_fd->Release());
                }
                else
                {
                    guard.unlock();
                    LOGT("release else sendQueueMut_");
                    async_fd->Release();
                }
            }
        };

        LOGT("get sendQueueMut_");
        std::unique_lock guard(sendQueueMut_);
        if (sendQueues_.find(async_fd->Get()) == sendQueues_.end())
        {
            LOGD("Send queues for fd=%d is absent. Finish sending.", async_fd->Get());
            return;
        }

        if (sendQueues_[fd].empty())
        {
            LOGE("QUEUE is empty somehow for fd=%d", fd);
            guard.unlock();
            LOGT("release if sendQueueMut_");
            return;
        }
        auto message = std::move(*sendQueues_[fd].begin());
        guard.unlock();
        LOGT("release sendQueueMut_");

        async_fd->SendMessage(std::move(message), cb);
    }

    void ConnectionManager::SendMessage(PeerId peer, Message &&msg)
    {
        LOGD("Start sending message");

        int fd = -1;
        {
            LOGT("get connToFdMut_");
            std::lock_guard guard(connToFdMut_);
            if (connToFd.count({peer, LinkDirection::OUTGOING}))
            {
                fd = connToFd[{peer, LinkDirection::OUTGOING}];
            }
            else if (connToFd.count({peer, LinkDirection::INCOMING}))
            {
                fd = connToFd[{peer, LinkDirection::INCOMING}];
            }
            LOGT("release connToFdMut_");
        }

        if (fd == -1)
        {
            LOGE("Cannot find connection for peer %s", PeerIdToString(peer).c_str());
            return;
        }

        {
            LOGT("get sendQueueMut_");
            std::lock_guard guard(sendQueueMut_);
            if (sendQueues_.find(fd) == sendQueues_.end())
            {
                LOGD("Send queues for fd=%d is absent. Finish sending.", fd);
                LOGT("release sendQueueMut_");
                return;
            }

            sendQueues_[fd].push_back(std::move(msg));
            if (sendQueues_[fd].size() > 1)
            {
                LOGD("Only put into the queue");
                LOGT("release sendQueueMut_");
                return;
            }
            LOGT("release sendQueueMut_");
        }

        SendMessageFd(fd);
    }

    void ConnectionManager::ReadMessage(int fd)
    {
        LOGD("Start waiting for new incoming message");
        auto recv_fd = AsyncFd::Create(fd, loop_);

        auto cb = [this, recv_fd](IoResult<Message> res)
        {
            LOGT("get activeConnsMut_");
            std::unique_lock guard(activeConnsMut_);
            auto find = activeConns.find(recv_fd->Get());
            if (find == activeConns.end())
            {
                LOGD("Connection is not active already. Finish reading from fd=%d.", recv_fd->Get());
                LOGT("release activeConnsMut_");
                return;
            }
            auto [peer, dir] = find->second;
            guard.unlock();
            LOGT("release activeConnsMut_");

            if (res.IsErr())
            {
                LOGE("Failed to read message from %s on fd %d", PeerIdToString(peer).c_str(), recv_fd->Get());
                CloseFD(recv_fd->Release());
                return;
            }

            LOGD("Received message from %s on fd %d", PeerIdToString(peer).c_str(), recv_fd->Get());
            ipTransport_->OnMessageReceived(peer, res.GetOk().value());
            ReadMessage(recv_fd->Release());
        };
        recv_fd->RecvMessage(cb);
    }

    void ConnectionManager::CloseFD(int fd)
    {
        LOGD("Closing socket: %d", fd);
        loop_->DeregisterFd(fd);
        LOGT("closing socket fd=%d", fd);
        close(fd);

        LOGT("get activeConnsMut_");
        std::unique_lock act_guard(activeConnsMut_);
        if (activeConns.count(fd))
        {
            auto conn = activeConns[fd];
            activeConns.erase(fd);
            act_guard.unlock();
            LOGT("release activeConnsMut_");

            {
                LOGT("get connToFdMut_");
                std::lock_guard guard(connToFdMut_);
                connToFd.erase(conn);
                LOGT("release connToFdMut_");
            }

            {
                LOGT("get sendQueueMut_");
                std::lock_guard guard(sendQueueMut_);
                sendQueues_.erase(fd);
                LOGT("release sendQueueMut_");
            }
            ipTransport_->OnLinkClosed(conn.first, conn.second);
        }
        LOGT("release activeConnsMut_");
    }

}
