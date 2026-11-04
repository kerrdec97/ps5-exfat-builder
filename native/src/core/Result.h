#pragma once

// Error handling for the whole backend.
//
// Engines never throw across module boundaries: every fallible operation
// returns fw::Result<T>. `Error::message` is written for the user (what
// happened + what to do, per the design system's voice rules); `detail` holds
// the technical cause and goes to the log only.

#include <QString>

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace fw {

enum class ErrorCode {
    Cancelled,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    InsufficientSpace,
    IoError,
    CorruptData,
    Unsupported,
    ExternalToolFailed,
    DependencyMissing,
    VerificationFailed,
    Timeout,
    Internal,
};

QString errorCodeName(ErrorCode code);

struct Error {
    ErrorCode code = ErrorCode::Internal;
    QString message; // user-facing
    QString detail;  // technical, log only

    Error() = default;
    Error(ErrorCode c, QString msg, QString det = {})
        : code(c), message(std::move(msg)), detail(std::move(det)) {}

    static Error cancelled() { return {ErrorCode::Cancelled, QStringLiteral("Cancelled.")}; }
    bool isCancelled() const { return code == ErrorCode::Cancelled; }

    // "message (detail)" for logs.
    QString toLogString() const;
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : m_v(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : m_v(std::in_place_index<1>, std::move(error)) {}

    bool ok() const { return m_v.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() & { return std::get<0>(m_v); }
    const T& value() const& { return std::get<0>(m_v); }
    T&& value() && { return std::get<0>(std::move(m_v)); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

    const Error& error() const { return std::get<1>(m_v); }
    Error takeError() { return std::get<1>(std::move(m_v)); }

    T valueOr(T fallback) const { return ok() ? value() : std::move(fallback); }

private:
    std::variant<T, Error> m_v;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)) {}

    bool ok() const { return !m_error.has_value(); }
    explicit operator bool() const { return ok(); }
    const Error& error() const { return *m_error; }
    Error takeError() { return std::move(*m_error); }

private:
    std::optional<Error> m_error;
};

using Status = Result<void>;

inline Status success() { return {}; }

} // namespace fw

// Propagate an error from an expression returning Result<...>.
#define FW_TRY(expr)                                                                       \
    do {                                                                                   \
        auto&& fw_try_result_ = (expr);                                                    \
        if (!fw_try_result_.ok())                                                          \
            return fw_try_result_.takeError();                                             \
    } while (0)

// Assign the value of a Result<T> or propagate its error.
#define FW_TRY_ASSIGN(lhs, expr)                                                           \
    auto&& FW_CONCAT_(fw_tmp_, __LINE__) = (expr);                                         \
    if (!FW_CONCAT_(fw_tmp_, __LINE__).ok())                                               \
        return FW_CONCAT_(fw_tmp_, __LINE__).takeError();                                  \
    lhs = std::move(FW_CONCAT_(fw_tmp_, __LINE__)).value()

#define FW_CONCAT_INNER_(a, b) a##b
#define FW_CONCAT_(a, b) FW_CONCAT_INNER_(a, b)
