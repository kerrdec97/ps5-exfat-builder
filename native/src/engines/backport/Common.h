#pragma once

// Shared pieces of the PS5 executable readers in this module: the size limit
// and the error codes they raise.

#include "core/Result.h"

#include <QString>

namespace fw::backport {

// Executables are read in memory; nothing larger is accepted.
inline constexpr qint64 kMaxExecBytes = 1024LL * 1024 * 1024;

// Error codes of the readers. They travel inside fw::Error::detail as a
// "[Code]" prefix so callers and tests can tell them apart.
enum class Code {
    Ok,
    ElfInvalid,
    ParamMissing,
    ParamSizeInvalid,
    ParamMagicInvalid,
    ParamOutOfRange,
    SelfEncrypted,
    SelfCompressed,
    SelfMalformed,
    SignUnsupported, // a valid ELF of a kind these readers do not handle (class, machine, type, entry sizes)
    Cancelled,
    Internal,
};

QString codeName(Code code);
ErrorCode toErrorCode(Code code);

// Builds an fw::Error whose detail starts with "[CodeName]".
Error makeError(Code code, const QString& message, const QString& detail = {});
// The Code carried by an error made with makeError (Internal otherwise;
// Cancelled for fw cancellation errors).
Code codeOf(const Error& error);

} // namespace fw::backport
