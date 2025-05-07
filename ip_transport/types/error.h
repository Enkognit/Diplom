#ifndef ERROR_H
#define ERROR_H

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "link_direction.h"
#include "logging.h"

namespace diplom {

enum class OpenSessionErrorKind {
    SESSION_EXISTS,
    NO_SUCH_PEER,
    ERROR,
};

constexpr static const char *ToString(OpenSessionErrorKind kind)
{
    switch (kind) {
        case OpenSessionErrorKind::SESSION_EXISTS:
            return "SESSION_EXISTS";
        case OpenSessionErrorKind::NO_SUCH_PEER:
            return "NO_SUCH_PEER";
        case OpenSessionErrorKind::ERROR:
            return "ERROR";
            break;
    }
}

struct OpenSessionError {
    OpenSessionErrorKind kind;
    LinkDirection dir;
};

[[nodiscard]] inline std::string ToString(OpenSessionError const &err)
{
    return std::string{ToString(err.kind)}.append(" ").append(ToString(err.dir));
}

using ErrorVariant = std::variant<OpenSessionError>;
struct Error : ErrorVariant {
    template <typename E> [[nodiscard]] constexpr bool Is() const
    {
        return std::holds_alternative<E>(static_cast<const ErrorVariant &>(*this));
    }

    template <typename E> [[nodiscard]] constexpr E As() const
    {
        return std::get<E>(static_cast<const ErrorVariant &>(*this));
    }

    template <typename... Fs> constexpr void Match(Fs &&...fs) const
    {
        struct : Fs... {
            using Fs::operator()...;
        } const visitor{std::forward<Fs>(fs)...};

        return std::visit(visitor, static_cast<const ErrorVariant &>(*this));
    }
};

[[nodiscard]] inline std::string ToString(Error const &error)
{
    if (error.Is<OpenSessionError>()) {
        return ToString(error.As<OpenSessionError>());
    }

    return "Unknown";
}

template <typename E, typename... Args> //
inline constexpr Error MakeError(Args &&...args)
{
    if constexpr (std::is_same_v<E, OpenSessionError>) {
        return Error{OpenSessionError{std::forward<Args>(args)...}};
    } else {
        throw std::runtime_error{"Wrong error type!"};
    }
}

}
#endif
