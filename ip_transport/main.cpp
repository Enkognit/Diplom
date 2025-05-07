#include <peer_id.h>
#include <ip_transport.h>
#include <random>
#include <transport_manager.h>
#include <logging.h>

using namespace diplom;

struct MyTransportManager : transport::BasicTransportManager {

    virtual void NotifyNewNeighbour(TransportId tid, PeerId pid) const {
        LOGI("Found neighbour pid: %s on tid: %d", PeerIdToString(pid).c_str());
    }
};

int main() {

    MyTransportManager manager;
    transport::ip::IpTransport transport;

    PeerId self;
    std::random_device dev;
    std::mt19937 rng(dev());
    std::uniform_int_distribution<uint8_t> d(0, 255);

    for (auto &i : self) {
        i = static_cast<std::byte>(d(rng));
    }

    transport.Init(self);

    manager.RegisterTransport(transport);

    transport.Start();
}