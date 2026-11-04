#pragma once

// Edits of an existing .ffpkg, the operations the legacy "Edit ffpkg" page
// ran through UFS2Tool (add, replace, delete, rename, plus mkdir, which the
// legacy page emulated with a .keep file). They are applied to the image's
// tree in memory; the job then builds a new image from it, verifies it and
// only then replaces the original (UFS2Tool edited the image in place).

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/ufs2/Ufs2Tree.h"

#include <QJsonArray>
#include <QStringList>

#include <vector>

namespace fw::ufs2 {

struct EditOp {
    enum class Kind { Add, Replace, Delete, Rename, Mkdir };
    Kind kind = Kind::Add;
    QString path;    // inside the image: "/sce_sys/param.json"
    QString source;  // Add, Replace: local file or folder
    QString newName; // Rename: new name in the same folder
};

QString editOpKindName(EditOp::Kind kind);
// [{"op":"add","path":"/a","source":"C:/x"}, {"op":"delete","path":"/b"}, ...]
Result<std::vector<EditOp>> parseEditOps(const QJsonArray& ops);
QJsonArray editOpsToJson(const std::vector<EditOp>& ops);

// Applies the operations in order:
//  add     - `source` (file, or folder copied recursively) becomes `path`;
//            missing parent folders are created; `path` must not exist.
//  replace - `path` must exist. A file gets the content of the file `source`;
//            a folder gets every file that exists in both it and the folder
//            `source` replaced, recursively (UFS2Tool's replace).
//  delete  - removes `path` (recursively).
//  rename  - renames `path` within its folder to `newName`.
//  mkdir   - creates the folder `path` and missing parents.
Status applyEdits(SourceTree& tree, const std::vector<EditOp>& ops, const CancellationToken& token,
                  QStringList* log = nullptr);

} // namespace fw::ufs2
