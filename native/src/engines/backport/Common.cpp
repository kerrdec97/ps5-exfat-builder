#include "engines/backport/Common.h"

namespace fw::backport {

QString codeName(Code code)
{
    switch (code) {
    case Code::Ok: return QStringLiteral("Ok");
    case Code::ElfInvalid: return QStringLiteral("ElfInvalid");
    case Code::ParamMissing: return QStringLiteral("ParamMissing");
    case Code::ParamSizeInvalid: return QStringLiteral("ParamSizeInvalid");
    case Code::ParamMagicInvalid: return QStringLiteral("ParamMagicInvalid");
    case Code::ParamOutOfRange: return QStringLiteral("ParamOutOfRange");
    case Code::SelfEncrypted: return QStringLiteral("SelfEncrypted");
    case Code::SelfCompressed: return QStringLiteral("SelfCompressed");
    case Code::SelfMalformed: return QStringLiteral("SelfMalformed");
    case Code::SignUnsupported: return QStringLiteral("SignUnsupported");
    case Code::Cancelled: return QStringLiteral("Cancelled");
    case Code::Internal: return QStringLiteral("Internal");
    }
    return QStringLiteral("Internal");
}

ErrorCode toErrorCode(Code code)
{
    switch (code) {
    case Code::Ok:
    case Code::Internal: return ErrorCode::Internal;
    case Code::ElfInvalid:
    case Code::ParamMissing:
    case Code::ParamSizeInvalid:
    case Code::ParamMagicInvalid:
    case Code::ParamOutOfRange:
    case Code::SelfMalformed: return ErrorCode::CorruptData;
    case Code::SelfEncrypted:
    case Code::SelfCompressed:
    case Code::SignUnsupported: return ErrorCode::Unsupported;
    case Code::Cancelled: return ErrorCode::Cancelled;
    }
    return ErrorCode::Internal;
}

Error makeError(Code code, const QString& message, const QString& detail)
{
    QString d = QStringLiteral("[%1]").arg(codeName(code));
    if (!detail.isEmpty())
        d += u' ' + detail;
    return Error(toErrorCode(code), message, d);
}

Code codeOf(const Error& error)
{
    if (error.isCancelled())
        return Code::Cancelled;
    const QString& d = error.detail;
    if (!d.startsWith(u'['))
        return Code::Internal;
    const qsizetype end = d.indexOf(u']');
    if (end < 0)
        return Code::Internal;
    const QString name = d.mid(1, end - 1);
    for (int c = int(Code::Ok); c <= int(Code::Internal); ++c)
        if (codeName(Code(c)) == name)
            return Code(c);
    return Code::Internal;
}

} // namespace fw::backport
