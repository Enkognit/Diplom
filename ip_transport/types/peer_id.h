#ifndef PEER_ID_H
#define PEER_ID_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

#include <securec.h>
#include <string>

#include "common.h"

namespace diplom {

constexpr size_t PEER_ID_LEN = 6;
constexpr size_t MAC_OCTET_LEN = 3;
constexpr size_t MAC_STR_LEN = 18;

// Using std::byte instead of uint8_t to avoid UB due to Strict Aliasing Rule
using PeerId = std::array<std::byte, PEER_ID_LEN>;

using PeerSeq = std::pair<PeerId, Sequence>;

[[maybe_unused]] static std::string PeerIdToString(PeerId const &peer_id)
{
    const char hex[] = "0123456789abcdef";
    std::byte const mask[] = {std::byte{0xF0}, std::byte{0x0F}};

    char s[MAC_STR_LEN];
    int ptr = 0;
    for (auto const &oct : peer_id) {
        s[ptr++] = hex[std::to_integer<int>(((oct & mask[0]) >> 4))];
        s[ptr++] = hex[std::to_integer<int>(oct & mask[1])];
        s[ptr] = (ptr == MAC_STR_LEN) ? '\0' : ':';
        ptr++;
    }

    return {s, MAC_STR_LEN - 1};
}

[[maybe_unused]] static PeerId MacStrToPeerId(const std::string &mac)
{
    PeerId id;
    for (size_t i = 0; i < mac.size(); i += MAC_OCTET_LEN) {
        std::string byte_str = mac.substr(i, MAC_OCTET_LEN - 1);
        auto value = strtoul(byte_str.c_str(), nullptr, 16);
        id[i / MAC_OCTET_LEN] = static_cast<std::byte>(value);
    }
    return id;
}

} 

template <> struct std::hash<diplom::PeerId> { // NOLINT
    size_t operator()(diplom::PeerId const &pid) const noexcept
    {
        uint64_t u64{};
        std::memcpy(&u64, pid.data(), std::min(sizeof(u64), pid.size()));
        return std::hash<uint64_t>{}(u64);
    }
};

inline bool operator<(diplom::PeerId const &a, diplom::PeerId const &b)
{
    return std::memcmp(std::data(a), std::data(b), std::size(a)) < 0;
}

inline bool operator==(diplom::PeerId const &a, diplom::PeerId const &b)
{
    return std::memcmp(std::data(a), std::data(b), std::size(a)) == 0;
}

template <char... bytes> //
constexpr std::array<std::byte, sizeof...(bytes)> operator""_as_bytes()
{
    return {std::byte{bytes}...};
}

#endif
