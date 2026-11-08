#pragma once

// Home › PS5 setup checklist (legacy PS5 Manager "Setup Checklist", MGR-5):
// five rows derived from the ps5.* settings, the last FTP diagnosis and the
// payload list, shown as "n / 5 complete".
//
//   ip        PS5 IP configured        ps5.consoleIp set
//   ftp       FTP reachable            the FTP browser is connected, or the last
//                                      ps5.ftp.diagnose for this address and port
//                                      reached the server and logged in
//   klog      Kernel log configured    address set, ps5.klogPort not the FTP port
//   payload   Payload port configured  address set, ps5.payloadPort not the FTP or
//                                      kernel-log port
//   payloads  Payloads added           at least one payload file in PS5 connect ›
//                                      Payloads exists (the native form of the
//                                      legacy payload folder, al_dir)
//
// The last diagnosis is kept in <data>/ps5-last-check.json so the row
// survives a restart.

#include "core/Settings.h"
#include "jobs/JobQueue.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QVariantMap>

#include <optional>

namespace fw::ui {

struct FtpCheckRecord {
    QString host;
    int port = 0;
    bool reachable = false;
    bool loggedIn = false;
    QDateTime when;
    QString detail; // the problem when it failed
    bool isFor(const QString& host, int port) const;
    bool passed() const { return reachable && loggedIn; }
};

QString lastFtpCheckPath();
std::optional<FtpCheckRecord> loadLastFtpCheck();
bool saveLastFtpCheck(const FtpCheckRecord& record);
// From a finished ps5.ftp.diagnose job (its outcome details); nullopt for
// other kinds, cancelled runs and jobs without an outcome.
std::optional<FtpCheckRecord> ftpCheckFromJob(const JobRecord& record);

struct ChecklistItem {
    QString id;
    bool done = false;
    QString label;   // the done or the pending wording
    QString detail;
    QString fixText; // link on a pending row ("Run diagnostics"); empty = none
    QString fixPage; // navigation target of the link
    QVariantMap fixArgs;
};

struct ChecklistInputs {
    QString consoleIp;
    int ftpPort = 2121;
    int klogPort = 3232;
    int payloadPort = 9021; // elfldr (9090 is etaHEN's package installer)
    std::optional<FtpCheckRecord> lastCheck;
    bool browserConnected = false; // FTP browser session open to consoleIp
    int payloadsReady = 0;         // payload list entries whose file exists
};

ChecklistInputs checklistInputs(const SettingsValues& settings);
QList<ChecklistItem> setupChecklist(const ChecklistInputs& in);
int completedCount(const QList<ChecklistItem>& items);

} // namespace fw::ui
