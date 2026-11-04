#pragma once

// SELF writer (fake signing).
//
// Port of make_fself.py by john-tornblom (ps5-payload-dev/sdk,
// samples/install_app/make_fself.py, GNU GPL version 3), as modified by
// Nazky (@NazkyYT). Byte-for-byte compatible with the Python writer for the
// parameters the app uses (docs/BACKPORT_PARITY.md §2.8, Appendix A).
// Distributed under the GNU GPL version 3.

#include "core/Result.h"
#include "engines/backport/SelfFormat.h"

#include <QByteArray>
#include <QByteArrayView>

namespace fw::backport {

struct SignParams {
    quint64 paid = self::kDefaultPaid;
    quint64 ptype = self::kPtypeFake;
    quint64 appVersion = 0;
    quint64 fwVersion = 0;
};

// Builds a fake-signed SELF from an ELF. Section headers are ignored
// (e_shnum becomes 0 in the copy). Errors: SignUnsupported (header rules,
// header too large), ElfInvalid (structure, truncated segment).
Result<QByteArray> makeFakeSelf(QByteArrayView elf, const SignParams& params = {});

} // namespace fw::backport
