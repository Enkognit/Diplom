#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <sys/types.h>
#include <vector>

#include "epoll_loop.h"
#include "ip_broadcast.h"
#include "ip_transport.h"
#include "ip_utils.h"
#include "logging.h"

#undef TAG
#define TAG "BroadcastManager"

namespace diplom::transport::ip {

int BroadcastManager::Init(IpTransport *ipTransport, std::shared_ptr<EpollLoop> &loop)
{
    if (!ipTransport) {
        LOGE("IpTransport is null, cannot initialize");
        return -1;
    }

    ipTransport_ = ipTransport;
    loop_ = loop;

    return 0;
}

/**
 * Gets IPv4 broadcast addresses for active, non-loopback interfaces on Linux.
 *
 * Scans through network interfaces using getifaddrs, calculates the broadcast
 * address for suitable IPv4 interfaces, and returns them as raw in_addr_t values.
 */
std::vector<in_addr_t> GetBroadcastAddresses()
{
    std::vector<in_addr_t> broadcast_addresses;
    struct ifaddrs *ifaddr_list = nullptr;
    struct ifaddrs *ifa = nullptr;

    // Get the list of interfaces
    if (getifaddrs(&ifaddr_list) == -1) {
        return {};
    }

    // Iterate through the linked list of interfaces
    for (ifa = ifaddr_list; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_netmask == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        // 2. Check interface flags: Must be UP, support BROADCAST, and NOT be LOOPBACK
        if (!((ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_BROADCAST) && !(ifa->ifa_flags & IFF_LOOPBACK))) {
            continue;
        }

        auto *addr_in = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
        auto *netmask_in = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_netmask);

        in_addr_t ip_addr_n = addr_in->sin_addr.s_addr;
        in_addr_t netmask_n = netmask_in->sin_addr.s_addr;

        // Calculate broadcast address: ip | (~netmask)
        // The calculation works correctly with network byte order values.
        in_addr_t broadcast_addr_n = ip_addr_n | (~netmask_n);

        // Add the calculated broadcast address (in network byte order) to the vector
        broadcast_addresses.push_back(broadcast_addr_n);
    }

    // Free the memory allocated by getifaddrs
    freeifaddrs(ifaddr_list);

    return broadcast_addresses;
}

bool BroadcastManager::Start()
{
    LOGD("Creating broadcast UDP socket");

    if ((broadcastFd_ = common::create_broadcast_socket()) == -1) {
        LOGE("Failed to create UDP broadcast socket");
        return false;
    }

    auto cb_br = [this](EpollResult res) { ReceiveBroadcasts(); };

    CancellationToken br_tok = 0;
    if (!loop_->RegisterAction(broadcastFd_, IoDirection::INPUT, cb_br, br_tok)) {
        LOGE("Failed to set callback for broadcast messages");
        close(broadcastFd_);
        return false;
    }

    LOGD("Start creating timer");

    if ((broadcastTimerFd_ = common::create_timer(BROADCAST_DELAY)) == -1) {
        LOGE("Failed to create broadcast timer");
        loop_->DeregisterAction(br_tok);
        close(broadcastFd_);
        return false;
    }

    auto cb_timer = [this](EpollResult res) {
        uint64_t u;
        read(broadcastTimerFd_, &u, sizeof(u));
        SendBroadcastHello();
        common::set_timer(broadcastTimerFd_, 1);
    };

    CancellationToken timer_tok = 0;
    if (!loop_->RegisterAction(broadcastTimerFd_, IoDirection::INPUT, cb_timer, timer_tok)) {
        LOGE("Failed to set callback for broadcast timer");
        loop_->DeregisterAction(br_tok);
        close(broadcastFd_);
        close(broadcastTimerFd_);
        return false;
    }

    // Send broadcast immediately after start, don't wait next timer invocation.
    SendBroadcastHello();

    return true;
}

void BroadcastManager::SendBroadcastHello()
{
    LOGD("Sending broadcast hello messages");
    HelloMsg msg = {.id = ipTransport_->_self};
    for (auto addr : GetBroadcastAddresses()) {
        char str[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &addr, str, INET_ADDRSTRLEN);
        LOGD("Sending broadcast to %s", str);

        if (!common::send_broadcast(broadcastFd_, (char const *)&msg, sizeof(msg), addr)) {
            LOGE("Failed to send broadcast to %s: %s", str, strerror(errno));
        }
    }
}

void BroadcastManager::ReceiveBroadcasts()
{
    LOGD("Receiving broadcast hello message");
    while (auto odata = common::read_broadcast(broadcastFd_)) {
        auto [message, addr] = odata.value();

        if (message.size() != sizeof(HelloMsg)) {
            LOGE("Received hello message has wrong size: %d instead of %d", message.size(), sizeof(HelloMsg));
            continue;
        }

        auto msg = (HelloMsg *)message.data();
        if (!msg->IsValid()) {
            LOGE("Received hello message is not valid");
            continue;
        }
        ProcessHello(addr, *msg);
    }
    LOGD("Done");
}

void BroadcastManager::ProcessHello(struct in_addr sender, HelloMsg const &msg, bool needNotify)
{
    LOGD("Start processing hello message. peer: %s addr: %s", PeerIdToString(msg.id).c_str(),
         common::in_addr_to_string(sender).c_str());

    if (msg.id == ipTransport_->_self) {
        LOGT("This is our message");
        return;
    }

    std::unique_lock lock(globalLock_);
    int tfd = peerToFd_[msg.id];
    lock.unlock();
    if (tfd) {
        LOGT("Set timer for peer %s", PeerIdToString(msg.id).c_str());
        common::set_timer(tfd, LOST_PEER_DELAY);
    } else {
        LOGT("Creating timer for peer %s", PeerIdToString(msg.id).c_str());

        tfd = common::create_timer(LOST_PEER_DELAY);
        if (tfd == -1) {
            LOGE("Failed to create timer");
            return;
        }

        auto cb = [this, tfd](EpollResult res) { ProcessPeerLost(tfd); };

        CancellationToken token = 0;
        if (!loop_->RegisterAction(tfd, IoDirection::INPUT, cb, token)) {
            LOGE("Failed to register action for neigbor timer");
            return;
        }

        {
            std::lock_guard guard(globalLock_);
            peerToFd_[msg.id] = tfd;
            fdToPeer_.emplace(tfd, std::make_pair(msg.id, token));
            peerToAddr_[msg.id] = sender;
            addrToPeer_[sender.s_addr] = msg.id;
        }

        LOGD("Found peer: %s on address %s", PeerIdToString(msg.id).c_str(), common::in_addr_to_string(sender).c_str());
        if (needNotify) {
            ipTransport_->OnNeighborFound(msg.id);
        }
    }
}

void BroadcastManager::ProcessPeerLost(int fd)
{
    uint64_t u;
    read(fd, &u, sizeof(u));

    std::unique_lock guard(globalLock_);

    auto find = fdToPeer_.find(fd);
    if (find == fdToPeer_.end()) {
        LOGE("Cannot find fd for lost peer timer");
        return;
    }

    auto [peer, token] = fdToPeer_[fd];
    auto addr = peerToAddr_[peer];

    LOGD("Lost peer %s", PeerIdToString(peer).c_str());
    loop_->DeregisterAction(token);
    peerToFd_.erase(peer);
    fdToPeer_.erase(fd);
    peerToAddr_.erase(peer);
    addrToPeer_.erase(addr.s_addr);
    guard.unlock();

    ipTransport_->OnNeighborLost(peer);
}

BroadcastManager::~BroadcastManager()
{
    // loop_->RemoveDescriptor(broadcastTimerFd_);
    // loop_->RemoveDescriptor(broadcastFd_);
    close(broadcastTimerFd_);
    close(broadcastFd_);
}

std::optional<in_addr> BroadcastManager::GetAddress(PeerId peer)
{
    std::lock_guard guard(globalLock_);

    auto find = peerToAddr_.find(peer);
    if (find == peerToAddr_.end()) {
        return std::nullopt;
    }
    return find->second;
}

std::optional<PeerId> BroadcastManager::GetPeer(in_addr addr)
{
    std::lock_guard guard(globalLock_);

    auto find = addrToPeer_.find(addr.s_addr);
    if (find == addrToPeer_.end()) {
        return std::nullopt;
    }
    return find->second;
}

} 
