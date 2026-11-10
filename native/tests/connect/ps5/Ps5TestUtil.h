#pragma once

// Shared by the ps5 tests: assertions on fw::Result, and the small fixtures
// every one of them needs (one copy, so they cannot drift apart).

#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpClient.h"
#include "connect/ps5/Module.h"
#include "core/Dependencies.h"
#include "core/Settings.h"
#include "jobs/Job.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTest>

#include <memory>

// Reading the wrong side of a Result throws (Result<T>) or is undefined
// behaviour (Status), which ends the whole test program instead of failing
// one test function. These look at ok() first, so an unexpected success or
// failure is an ordinary FAIL line with the reason.

// `result` must be an error with this code.
#define COMPARE_ERROR_CODE(result, expectedCode)                                                                       \
    do {                                                                                                               \
        const auto& fw_checked_ = (result);                                                                            \
        QVERIFY2(!fw_checked_.ok(), "succeeded, but an error was expected: " #result);                                 \
        QCOMPARE(fw_checked_.error().code, expectedCode);                                                              \
    } while (false)

// `result` must have succeeded with this value.
#define COMPARE_VALUE(result, expected)                                                                                \
    do {                                                                                                               \
        const auto& fw_checked_ = (result);                                                                            \
        QVERIFY2(fw_checked_.ok(), fw_checked_.ok() ? "" : qPrintable(fw_checked_.error().toLogString()));             \
        QCOMPARE(fw_checked_.value(), expected);                                                                       \
    } while (false)

namespace ps5test {

// The whole file; empty when it cannot be read.
inline QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Short timeouts: the fakes answer at once, and a test that goes wrong
// should fail soon.
inline fw::ps5::FtpOptions quick()
{
    fw::ps5::FtpOptions o;
    o.connectTimeoutMs = 3000;
    o.replyTimeoutMs = 3000;
    o.idleTimeoutMs = 3000;
    return o;
}

// A console on 127.0.0.1 whose FTP server is the fake on `ftpPort`.
inline fw::ps5::ConsoleEndpoint endpoint(quint16 ftpPort)
{
    fw::ps5::ConsoleEndpoint ep;
    ep.host = QStringLiteral("127.0.0.1");
    ep.ftpPort = ftpPort;
    ep.connectTimeoutMs = 3000;
    ep.idleTimeoutMs = 3000;
    return ep;
}

// The ps5 module registered into a schema, a registry and a job factory of its own.
struct Module {
    std::shared_ptr<fw::SettingsSchema> schema = std::make_shared<fw::SettingsSchema>();
    fw::DependencyRegistry deps;
    fw::JobFactory jobs;
    Module()
    {
        fw::ModuleContext ctx{*schema, deps, jobs};
        fw::ps5::registerModule(ctx);
    }
};

} // namespace ps5test
