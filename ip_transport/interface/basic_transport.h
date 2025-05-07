#ifndef BASIC_TRANSPORT_H
#define BASIC_TRANSPORT_H

#include <climits>

#include "common.h"
#include "link_direction.h"
#include "message.h"
#include "peer_id.h"

namespace diplom::transport {

class BasicTransportManager;

class BasicTransport {
protected:
    BasicTransportManager *_manager{nullptr};
    PeerId _self;

public:
    BasicTransport() : _self() {}

    int Init(PeerId peer_id)
    {
        _self = peer_id;
        return 0;
    }

    virtual void InjectTransportManager(BasicTransportManager *manager)
    {
        _manager = manager;
    }

    [[nodiscard]] virtual TransportId GetTransportId() const noexcept = 0;

    virtual void OpenSession(PeerId) = 0;

    virtual void CloseSession(PeerId, LinkDirection) = 0;

    virtual void SendMessage(PeerId, Message &&) = 0;

    virtual ~BasicTransport() = default;
};

} 
#endif
