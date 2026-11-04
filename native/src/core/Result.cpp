#include "core/Result.h"

namespace fw {

QString errorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::Cancelled: return QStringLiteral("cancelled");
    case ErrorCode::InvalidArgument: return QStringLiteral("invalid-argument");
    case ErrorCode::NotFound: return QStringLiteral("not-found");
    case ErrorCode::AlreadyExists: return QStringLiteral("already-exists");
    case ErrorCode::PermissionDenied: return QStringLiteral("permission-denied");
    case ErrorCode::InsufficientSpace: return QStringLiteral("insufficient-space");
    case ErrorCode::IoError: return QStringLiteral("io-error");
    case ErrorCode::CorruptData: return QStringLiteral("corrupt-data");
    case ErrorCode::Unsupported: return QStringLiteral("unsupported");
    case ErrorCode::ExternalToolFailed: return QStringLiteral("external-tool-failed");
    case ErrorCode::DependencyMissing: return QStringLiteral("dependency-missing");
    case ErrorCode::VerificationFailed: return QStringLiteral("verification-failed");
    case ErrorCode::Timeout: return QStringLiteral("timeout");
    case ErrorCode::Internal: return QStringLiteral("internal");
    }
    return QStringLiteral("unknown");
}

QString Error::toLogString() const
{
    QString s = QStringLiteral("[%1] %2").arg(errorCodeName(code), message);
    if (!detail.isEmpty())
        s += QStringLiteral(" (%1)").arg(detail);
    return s;
}

} // namespace fw
