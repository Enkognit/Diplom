#ifndef LINK_DATA_H
#define LINK_DATA_H

#include <cstring>
#include <tuple>

#include "link_direction.h"
#include "serializable.h"

namespace diplom {

struct LinkData {
    LinkDirection dir{};
};

DERIVE_SERIALIZABLE(diplom::LinkData, 1);

inline bool operator==(LinkData const &lhs, LinkData const &rhs) noexcept
{
    return std::tie(lhs.dir) == std::tie(rhs.dir);
}

} 

#endif
