#ifndef IP_BROADCAST_H
#define IP_BROADCAST_H

#include <cstring>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <sys/epoll.h>

#include "epoll_loop.h"
#include "ip_utils.h"

namespace diplom::transport::ip {

class IpTransport;

/**
 * Broadcast manager does 2 things:
 * - periodically (every 1 second) send broadcast messages via UDP;
 * - keep track of visible peers.
 *
 * Peer node can become visible by us in 2 situations:
 * - we received broadcast UDP Hello::Notify message from it;
 * - we received incoming TCP connetion from it and then received Hello::Notify message.
 *
 * In both situations `ProcessHello` function is called.
 *
 * Peer node is treated as lost if we did not receive UDP broadcast message from him
 * during 10 seconds.
 */

struct BroadcastManager {
public:
    BroadcastManager() : ipTransport_() {}

    int Init(IpTransport *ipTransport, std::shared_ptr<EpollLoop> &loop);

    ~BroadcastManager();

    bool Start();

    void SendBroadcastHello();

    void ReceiveBroadcasts();

    void ProcessHello(struct in_addr senderAddr, HelloMsg const &msg, bool needNotify = true);

    void ProcessPeerLost(int fd);

    std::optional<in_addr> GetAddress(PeerId peer);
    std::optional<PeerId> GetPeer(in_addr addr);

private:
    IpTransport *ipTransport_;
    std::shared_ptr<EpollLoop> loop_;

    int broadcastTimerFd_ = -1;
    int broadcastFd_ = -1;

    std::mutex globalLock_;
    std::map<int, std::pair<PeerId, CancellationToken>> fdToPeer_;
    std::map<PeerId, int> peerToFd_;

    std::map<PeerId, in_addr> peerToAddr_;
    std::map<in_addr_t, PeerId> addrToPeer_;

    friend IpTransport;
};
} 

#endif
