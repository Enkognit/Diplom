#ifndef IP_TRANSPORT_H
#define IP_TRANSPORT_H

#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <thread>

#include "basic_transport.h"
#include "common.h"
#include "epoll_loop.h"
#include "error.h"
#include "ip_broadcast.h"
#include "ip_conn_manager.h"

namespace diplom::transport::ip {

/**
 * IpTransport does next things:
 * - runs loop around epoll_wait in separate thread (epoll-thread);
 * - executes commands from TransportManager.
 *
 * Notifications from IO to TransportManager are executed in context of epoll-thread.
 * Commands from TransportManager are executed in context of TransportManager thread.
 *
 * Access to BroadcastManager and ConnectionManager should be done holding
 * corresponding mutex.
 */

class IpTransport : public BasicTransport {

public:
    IpTransport() = default;

    int Init(PeerId localPeerId);

    bool Start();

    [[nodiscard]] TransportId GetTransportId() const noexcept override;

    void OpenSession(PeerId peer_id) override;

    void CloseSession(PeerId peer_id, LinkDirection direction) override;

    void SendMessage(PeerId peer_id, Message &&message) override;

    ~IpTransport() override;

private:
    void OnNeighborFound(PeerId peer);
    void OnNeighborLost(PeerId peer);
    void OnLinkEstablishing(PeerId peer, LinkDirection dir);
    void OnLinkEstablishingError(PeerId peer, LinkDirection dir,
                                 OpenSessionErrorKind kind = OpenSessionErrorKind::ERROR);
    void OnLinkClosed(PeerId peer, LinkDirection dir);
    void OnMessageReceived(PeerId peer, Message &&msg);

    std::optional<in_addr> GetAddress(PeerId peer);
    std::optional<PeerId> GetPeer(in_addr addr);
    void PrintConnStats();

    std::shared_ptr<EpollLoop> loop_;

    BroadcastManager brManager;
    std::mutex _brMutex;

    ConnectionManager connManager;
    std::mutex _connMutex;

    friend struct BroadcastManager;
    friend struct ConnectionManager;

    std::thread epollThread;
};

} 

#endif
