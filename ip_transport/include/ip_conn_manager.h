#ifndef IP_CONN_MANAGER_H
#define IP_CONN_MANAGER_H

#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "epoll_loop.h"
#include "link_direction.h"
#include "message.h"
#include "peer_id.h"

namespace diplom::transport::ip {

class IpTransport;

/**
 * Connection manager does next operations:
 * - receives incoming TCP connections;
 * - establishes outgoing TCP connections in context of TransportManager.
 *
 * # Connection establishment mechanism
 *
 * ## Incoming connection
 * - Accept incoming TCP connection.
 * - Put connection to `pending` list.
 * - Read Hello::Notify message.
 * - Call BroadcastManager to process hello message.
 * - Send Hello::Answer message.
 * - Move connection from `pending` list to `active` list.
 * - Notify TransportManager about new connection.
 *
 * If any error happens in this path, connection just closed without any
 * notifications to TransportManager.
 *
 * ## Outgoing connection
 * - Connect to the remote host via TCP.
 * - Put connection to `pending` list.
 * - Wait till connection is established (no timeout for this).
 * - Send Hello::Notify message.
 * - Read Hello::Answer message.
 * - Move connection from `pending` list to `active` list.
 * - Notify TransportManager about new connection.
 *
 * If any error happens in this path, connection is closed and
 * coresponding notification is sent to TransportManager.
 */

struct ConnectionManager {
public:
    ConnectionManager() : ipTransport_() {}

    int Init(IpTransport *ipTransport, std::shared_ptr<EpollLoop> &loop);

    bool Start();

    void EstablishConnection(PeerId peer);

    void CloseConn(PeerId peer, LinkDirection dir);

    void SendMessage(PeerId peer_id, Message &&msg);

private:
    void AcceptConn();
    void IncomingHelloExchange(int fd);
    void OutgoingHelloExchange(int fd, PeerId peer);

    void CloseFD(int fd);
    void ReadMessage(int fd);
    void SendMessageFd(int fd);

    IpTransport *ipTransport_;
    friend IpTransport;
    std::shared_ptr<EpollLoop> loop_;

    int listenFd_ = -1;

    std::map<int, std::pair<PeerId, LinkDirection>> activeConns;
    std::mutex activeConnsMut_;

    std::map<std::pair<PeerId, LinkDirection>, int> connToFd;
    std::mutex connToFdMut_;

    std::unordered_map<int, std::deque<Message>> sendQueues_;
    std::mutex sendQueueMut_;
};

} 

#endif
