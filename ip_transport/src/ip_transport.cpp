#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <sys/epoll.h>
#include <thread>

#include "basic_transport.h"
#include "common.h"
#include "epoll_loop.h"
#include "error.h"
#include "ip_transport.h"
#include "logging.h"
#include "transport_manager.h"

#undef TAG
#define TAG "IpTransport"

namespace diplom::transport::ip {

int IpTransport::Init(PeerId localPeerId)
{
    BasicTransport::Init(localPeerId);
    loop_ = std::make_shared<EpollLoop>();
    brManager.Init(this, loop_);
    connManager.Init(this, loop_);
    return 0;
}

IpTransport::~IpTransport()
{
    loop_->Stop();
    if (epollThread.joinable()) {
        epollThread.join();
    }
}

void IpTransport::PrintConnStats()
{
    std::unique_lock guard(_connMutex);

    LOGD("---------------");
    LOGD("Active conns: %zu", connManager.activeConns.size());
    for (auto [_, peer_con] : connManager.activeConns) {
        auto [peer, dir] = peer_con;
        LOGD("%s %s", ToString(dir), PeerIdToString(peer).c_str());
    }
    LOGD("---------------");
}

bool IpTransport::Start()
{
    if (!loop_->Init()) {
        LOGE("Failed to initialize epoll loop");
        return false;
    }

    if (!(brManager.Start() && connManager.Start())) {
        LOGE("Cannot initialize broadcast and connection managers");
        return false;
    }

    epollThread = std::thread([this]() {
        // PrintConnStats();
        loop_->Run();
    });

    return true;
}

[[nodiscard]] TransportId IpTransport::GetTransportId() const noexcept
{
    return 1;
}

void IpTransport::OpenSession(PeerId peer_id)
{
    std::unique_lock guard(_connMutex);
    connManager.EstablishConnection(peer_id);
}

void IpTransport::SendMessage(PeerId peer_id, Message &&message)
{
    LOGD("Sending message to peer %s", PeerIdToString(peer_id).c_str());
    std::unique_lock guard(_connMutex);
    connManager.SendMessage(peer_id, std::move(message));
}

void IpTransport::CloseSession(PeerId peer_id, LinkDirection direction)
{
    std::unique_lock guard(_connMutex);
    connManager.CloseConn(peer_id, direction);
}

void IpTransport::OnNeighborFound(PeerId peer)
{
    LOGD("Found new neighbor: %s", PeerIdToString(peer).c_str());
    _manager->NotifyNewNeighbour(GetTransportId(), peer);
}

void IpTransport::OnNeighborLost(PeerId peer)
{
    LOGD("Lost neighbor: %s", PeerIdToString(peer).c_str());
    _manager->NotifyLostNeighbour(GetTransportId(), peer);
}

void IpTransport::OnLinkEstablishing(PeerId peer, LinkDirection dir)
{
    LOGD("Session opened with peer %s", PeerIdToString(peer).c_str());
    _manager->NotifySessionOpened(GetTransportId(), peer, dir);
}

void IpTransport::OnLinkEstablishingError(PeerId peer, LinkDirection dir, OpenSessionErrorKind kind)
{
    LOGD("Failed to open session with peer %s", PeerIdToString(peer).c_str());
    _manager->NotifyError(GetTransportId(), peer, MakeError<OpenSessionError>(kind, dir));
}

void IpTransport::OnLinkClosed(PeerId peer, LinkDirection dir)
{
    LOGD("Session closed with peer %s", PeerIdToString(peer).c_str());
    _manager->NotifySessionLost(GetTransportId(), peer, dir);
}

void IpTransport::OnMessageReceived(PeerId peer, Message &&msg)
{
    LOGD("Message received from peer %s", PeerIdToString(peer).c_str());
    _manager->ReceivedMessage(GetTransportId(), peer, std::move(msg));
}

std::optional<in_addr> IpTransport::GetAddress(PeerId peer)
{
    std::unique_lock guard(_brMutex);
    return brManager.GetAddress(peer);
}

std::optional<PeerId> IpTransport::GetPeer(in_addr addr)
{
    std::unique_lock guard(_brMutex);
    return brManager.GetPeer(addr);
}

} 
