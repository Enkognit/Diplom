#include <ip_transport.h>
#include <logging.h>
#include <peer_id.h>
#include <transport_manager.h>
#include <unistd.h>
#include <random>
#include "link_direction.h"
#include "message.h"

#undef TAG
#define TAG "main"

using namespace diplom;

struct TransportManager : transport::BasicTransportManager {

    void NotifyNewNeighbour(TransportId tid, PeerId pid) override {
        LOGI("Found neighbour pid: %s on tid: %d", PeerIdToString(pid).c_str(),
             tid);
        OpenSession(tid, pid);
    }

    void NotifyLostNeighbour(TransportId tid, PeerId pid) override {
        LOGI("Lost neighbour pid: %s on tid: %d", PeerIdToString(pid).c_str(),
             tid);
    }

    void NotifySessionOpened(TransportId tid, PeerId pid,
                             LinkDirection ldir) override {
        LOGI("Got %s connection to neighbour pid: %s on tid: %d",
             ToString(ldir), PeerIdToString(pid).c_str(), tid);
        // Message msg{std::byte(1)};
        // SendMessage(tid, pid, std::move(msg));
    }

    void NotifySessionLost(TransportId tid, PeerId pid,
                           LinkDirection ldir) override {
        LOGI("Lost %s connection to neighbour pid: %s on tid: %d",
             ToString(ldir), PeerIdToString(pid).c_str(), tid);
    }
};

int main(int argc, char** argv) {

    if (argc != 3) {
        LOGI("Must be 2 arguments");
        return 1;
    }

    int i = std::atoi(argv[1]);
    int duration = std::atoi(argv[2]);

    TransportManager manager;
    transport::ip::IpTransport transport;

    PeerId self{};
    std::random_device dev;
    std::mt19937 rng(dev());
    std::uniform_int_distribution<uint8_t> d(0, 255);

    for (auto& j : self) {
        j = static_cast<std::byte>(0);
    }

    self.back() = static_cast<std::byte>(i);

    transport.Init(self);

    manager.RegisterTransport(transport);

    transport.Start();

    sleep(duration);
}