#include "engines/backport/NidPatch.h"

namespace fw::backport {

const std::vector<NidSubstitution>& nidSubstitutions()
{
    static const std::vector<NidSubstitution> table = {
        {QByteArrayLiteral("4h6F1LLbTiw#A#B"), QByteArrayLiteral("IWIBBdTHit4#A#B"), 6},
    };
    return table;
}

QString nidActionName(NidAction action)
{
    switch (action) {
    case NidAction::None: return QStringLiteral("none");
    case NidAction::Applied: return QStringLiteral("applied");
    case NidAction::Reverted: return QStringLiteral("reverted");
    case NidAction::AlreadyPatched: return QStringLiteral("already_patched");
    case NidAction::Disabled: return QStringLiteral("disabled");
    }
    return QStringLiteral("none");
}

namespace {
int replaceAll(QByteArray& data, const QByteArray& from, const QByteArray& to)
{
    // Non-overlapping, left to right; `from` and `to` have the same length.
    int n = 0;
    qsizetype pos = 0;
    while ((pos = data.indexOf(from, pos)) >= 0) {
        data.replace(pos, from.size(), to);
        pos += to.size();
        ++n;
    }
    return n;
}
} // namespace

NidResult applyNid(QByteArray& data, int targetSdk, NidMode mode)
{
    NidResult r;
    if (mode == NidMode::Off) {
        r.action = NidAction::Disabled;
        return r;
    }
    for (const NidSubstitution& s : nidSubstitutions()) {
        const bool hasFrom = data.contains(s.from);
        const bool hasTo = data.contains(s.to);
        if (targetSdk <= s.boundarySdk) {
            if (!hasFrom)
                continue;
            if (hasTo) {
                r.action = NidAction::AlreadyPatched;
                continue;
            }
            r.count += replaceAll(data, s.from, s.to);
            r.action = NidAction::Applied;
        } else {
            if (hasTo && !hasFrom) {
                r.count += replaceAll(data, s.to, s.from);
                r.action = NidAction::Reverted;
            }
        }
    }
    return r;
}

void reverseNid(QByteArray& data, const NidResult& result)
{
    for (const NidSubstitution& s : nidSubstitutions()) {
        if (result.action == NidAction::Applied)
            replaceAll(data, s.to, s.from);
        else if (result.action == NidAction::Reverted)
            replaceAll(data, s.from, s.to);
    }
}

} // namespace fw::backport
