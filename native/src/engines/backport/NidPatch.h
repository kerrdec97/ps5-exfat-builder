#pragma once

// libc import NID substitution (docs/BACKPORT_PARITY.md §2.9). Data-driven
// from the documented facts: the two 15-byte ASCII strings and the SDK 6
// boundary. Applied after signing, like the Python engine (byte parity with
// Appendix A.2). Which libc functions the NIDs name is UNVERIFIED.

#include <QByteArray>
#include <QString>

#include <vector>

namespace fw::backport {

struct NidSubstitution {
    QByteArray from;      // "original" pattern
    QByteArray to;        // "replacement" pattern
    int boundarySdk = 6;  // target <= boundary: apply; target > boundary: revert
};

const std::vector<NidSubstitution>& nidSubstitutions();

enum class NidMode { Auto, Off };
enum class NidAction { None, Applied, Reverted, AlreadyPatched, Disabled };

QString nidActionName(NidAction action);

struct NidResult {
    NidAction action = NidAction::None;
    int count = 0; // occurrences replaced
};

// Target SDK <= 6: when the data holds `from` but not `to`, every `from`
// becomes `to` (both present: AlreadyPatched, unchanged). Target SDK > 6:
// when the data holds `to` and not `from`, every `to` becomes `from`.
NidResult applyNid(QByteArray& data, int targetSdk, NidMode mode);

// Undoes what applyNid did (exact, because the replaced-into pattern was
// absent before the substitution). Used to compare a staged SELF with the
// patched ELF during build verification.
void reverseNid(QByteArray& data, const NidResult& result);

} // namespace fw::backport
