#include "connect/ps5/RemoteTree.h"

#include "connect/ps5/RemotePath.h"

#include <algorithm>

namespace fw::ps5 {

namespace {

struct Pending {
    QString abs;
    QString rel;
};

} // namespace

Result<RemoteTree> walkRemoteTree(FtpClient& c, const QString& rootIn, const RemoteWalkOptions& options,
                                  const CancellationToken& token, const RemoteWalkProgress& progress)
{
    RemoteTree tree;
    tree.root = cleanRemotePath(rootIn);
    if (!tree.root.startsWith(u'/'))
        tree.root.prepend(u'/');

    FW_TRY_ASSIGN(FtpFeatures feats, c.features());
    bool useMlsd = feats.mlsd;
    bool trySize = options.fillMissingSizes;
    bool tryMdtm = options.exactTimes;

    // MLSD while the server takes it (exact times); LIST once it refuses MLSD.
    auto listDir = [&](const QString& path, bool& exact) -> Result<std::vector<FtpEntry>> {
        if (useMlsd) {
            auto r = c.list(path, FtpListMode::Mlsd, token);
            if (r.ok()) {
                exact = true;
                return r;
            }
            if (r.error().code != ErrorCode::Unsupported || !c.isConnected())
                return r;
            useMlsd = false;
        }
        exact = false;
        return c.list(path, FtpListMode::List, token);
    };

    std::vector<Pending> stack{{tree.root, QString()}};
    while (!stack.empty()) {
        if (!token.checkpoint())
            return Error::cancelled();
        const Pending cur = stack.back();
        stack.pop_back();
        bool exact = false;
        auto listed = listDir(cur.abs, exact);
        if (!listed.ok()) {
            if (listed.error().isCancelled() || !c.isConnected() || cur.rel.isEmpty())
                return listed.takeError();
            tree.unreadableDirs << cur.rel; // legacy: skip an unreadable sub-folder
            continue;
        }
        ++tree.folders;
        std::vector<FtpEntry> entries = std::move(listed.value());
        std::stable_sort(entries.begin(), entries.end(), [](const FtpEntry& a, const FtpEntry& b) {
            return a.name.toLower() < b.name.toLower();
        });
        std::vector<Pending> children;
        for (const FtpEntry& e : entries) {
            const QString rel = cur.rel.isEmpty() ? e.name : cur.rel + u'/' + e.name;
            const auto abs = remoteChild(cur.abs, e.name);
            if (!abs) {
                tree.skippedNames << rel;
                continue;
            }
            if (e.isLink) {
                ++tree.links;
                continue;
            }
            if (e.isDir) {
                tree.dirs << rel;
                children.push_back({*abs, rel});
                continue;
            }
            RemoteFile f;
            f.relPath = rel;
            f.size = e.size;
            f.modified = e.modified;
            f.exactTime = exact && e.modified.isValid();
            if (trySize && f.size < 0) {
                if (!token.checkpoint())
                    return Error::cancelled();
                auto s = c.size(*abs);
                if (s.ok())
                    f.size = s.value();
                else if (!c.isConnected())
                    return s.takeError();
                else if (s.error().code == ErrorCode::Unsupported)
                    trySize = false;
            }
            if (tryMdtm && !f.exactTime) {
                if (!token.checkpoint())
                    return Error::cancelled();
                auto t = c.mdtm(*abs);
                if (t.ok()) {
                    f.modified = t.value();
                    f.exactTime = true;
                } else if (!c.isConnected()) {
                    return t.takeError();
                } else if (t.error().code == ErrorCode::Unsupported) {
                    tryMdtm = false;
                }
            }
            if (f.size > 0)
                tree.totalBytes += f.size;
            tree.files.push_back(std::move(f));
        }
        // Depth first, sub-folders in name order (legacy collect_ftp_rows).
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            stack.push_back(*it);
        if (progress)
            progress(tree.folders, int(tree.files.size()));
    }
    return tree;
}

} // namespace fw::ps5
