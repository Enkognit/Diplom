#ifndef IO_RESULT_H
#define IO_RESULT_H

#include <optional>
#include <variant>

enum class IoError {
    Timeout,
    Error,
};

template <typename T> class IoResult {
public:
    static IoResult Ok(T value)
    {
        return IoResult(std::move(value));
    }

    static IoResult Err(IoError err)
    {
        return IoResult(err);
    }

    [[nodiscard]] bool IsOk() const
    {
        return std::holds_alternative<T>(result_);
    }

    [[nodiscard]] bool IsErr() const
    {
        return std::holds_alternative<IoError>(result_);
    }

    std::optional<T> GetOk()
    {
        if (IsOk()) {
            return std::get<T>(result_);
        }
        return std::nullopt;
    }

    std::optional<IoError> GetErr()
    {
        if (IsErr()) {
            return std::get<IoError>(result_);
        }
        return std::nullopt;
    }

private:
    explicit IoResult(T value) : result_(std::move(value)){};
    explicit IoResult(IoError err) : result_(err){};

    std::variant<T, IoError> result_;
};

#endif
