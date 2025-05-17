#ifndef TRANSPORT_MANAGER_H
#define TRANSPORT_MANAGER_H

#include <memory>
#include <thread>
#include <utility>

#include "basic_transport.h"
#include "common.h"
#include "error.h"
#include "peer_id.h"

namespace diplom::transport {

class BasicTransportManager {
public:
    BasicTransportManager() = default;

    ~BasicTransportManager() = default;

    void RegisterTransport(BasicTransport & transport) {
        transport.InjectTransportManager(this);
        _transports[transport.GetTransportId()] = &transport;
    }

    void EraseTransport(BasicTransport & transport) {
        _transports.erase(transport.GetTransportId());
    }

    // Transport -> Instance

    virtual void NotifyNewNeighbour(TransportId, PeerId) {}

    virtual void NotifyLostNeighbour(TransportId, PeerId) {}

    virtual void NotifySessionOpened(TransportId, PeerId, LinkDirection) {}

    virtual void NotifyError(TransportId, PeerId, Error) {}

    virtual void NotifySessionLost(TransportId, PeerId, LinkDirection) {}

    virtual void ReceivedMessage(TransportId, PeerId, Message &&) {}

    // User -> Transport

    virtual void OpenSession(TransportId tid, PeerId pid) {
        _transports.at(tid)->OpenSession(pid);
    }

    virtual void CloseSession(TransportId tid, PeerId pid, LinkDirection ldir) {
        _transports.at(tid)->CloseSession(pid, ldir);
    }

    virtual void SendMessage(TransportId tid, PeerId pid, Message && msg) {
        if (tid == TRANSPORT_ANY) {
            tid = _transports.begin()->second->GetTransportId();
        }
        _transports.at(tid)->SendMessage(pid, std::move(msg));
    }

private:

    std::unordered_map<TransportId, BasicTransport *> _transports;
};
} 

#endif
