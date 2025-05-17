#ifndef MESSAGE_H
#define MESSAGE_H

#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace diplom {
using Message = std::vector<std::byte>;

enum class MessageType : std::underlying_type_t<std::byte> {
    PACKET = 0,
    SYNC_ADV, //
    SYNC_REQ,
    SYNC_RESP,
};

[[maybe_unused]] constexpr static char const *ToString(MessageType tip)
{
    switch (tip) {
        case MessageType::PACKET:
            return "PACKET";
        case MessageType::SYNC_ADV:
            return "SYNC_ADV";
        case MessageType::SYNC_REQ:
            return "SYNC_REQ";
        case MessageType::SYNC_RESP:
            return "SYNC_RESP";
    }
}

[[maybe_unused]] static MessageType GetMessageType(Message const &msg)
{
    return static_cast<MessageType>(msg.at(0));
}

[[maybe_unused]] static std::string MessageToString(const Message &bytes)
{
    std::ostringstream oss;
    for (const auto &byte : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

}

#endif
