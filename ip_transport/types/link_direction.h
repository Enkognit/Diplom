#ifndef LINKDIR_H
#define LINKDIR_H

#include <common.h>
#include <type_traits>

namespace diplom {

enum class LinkDirection : std::underlying_type_t<std::byte> {
    INCOMING = 0,
    OUTGOING,
    ALL,
};

[[maybe_unused]] constexpr static const char *ToString(LinkDirection dir)
{
    switch (dir) {
        case LinkDirection::INCOMING:
            return "INCOMING";
        case LinkDirection::OUTGOING:
            return "OUTGOING";
        case LinkDirection::ALL:
            return "ALL";
    }
}

}

#endif
