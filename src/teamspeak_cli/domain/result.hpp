#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>

namespace teamspeak_cli::domain {

enum class ExitCode {
    ok = 0,
    usage = 2,
    config = 3,
    sdk = 4,
    connection = 5,
    not_found = 6,
    unsupported = 7,
    internal = 10,
};

struct Error {
    std::string category;
    std::string code;
    std::string message;
    ExitCode exit_code = ExitCode::internal;
    std::map<std::string, std::string> details;
};

template <typename T>
class Result {
  public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] auto ok() const -> bool { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const { return ok(); }

    [[nodiscard]] auto value() & -> T& { return *value_; }
    [[nodiscard]] auto value() const& -> const T& { return *value_; }
    [[nodiscard]] auto value() && -> T&& { return std::move(*value_); }

    [[nodiscard]] auto error() const -> const Error& { return *error_; }

  private:
    std::optional<T> value_;
    std::optional<Error> error_;
};

template <>
class Result<void> {
  public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] auto ok() const -> bool { return !error_.has_value(); }
    [[nodiscard]] explicit operator bool() const { return ok(); }
    [[nodiscard]] auto error() const -> const Error& { return *error_; }

  private:
    std::optional<Error> error_;
};

inline auto make_error(
    std::string category,
    std::string code,
    std::string message,
    ExitCode exit_code
) -> Error {
    return Error{
        .category = std::move(category),
        .code = std::move(code),
        .message = std::move(message),
        .exit_code = exit_code,
        .details = {},
    };
}

template <typename T>
inline auto ok(T value) -> Result<T> {
    return Result<T>(std::move(value));
}

inline auto ok() -> Result<void> {
    return Result<void>();
}

template <typename T>
inline auto fail(Error error) -> Result<T> {
    return Result<T>(std::move(error));
}

inline auto fail(Error error) -> Result<void> {
    return Result<void>(std::move(error));
}

}  // namespace teamspeak_cli::domain
