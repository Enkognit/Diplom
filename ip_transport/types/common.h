#ifndef COMMON_H
#define COMMON_H

#include <cstddef>
#include <cstring>

#include <cstdint>
#include <type_traits>

namespace diplom {

using TransportId = std::underlying_type_t<std::byte>;
using Sequence = uint64_t;

constexpr TransportId TRANSPORT_ANY = UINT8_MAX;

enum class SessionStatus { OPEN = 0, CLOSE };

}

#endif
