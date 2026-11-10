#pragma once

// Assertions on fw::Result for the ps5 tests.
//
// Reading the wrong side of a Result throws (Result<T>) or is undefined
// behaviour (Status), which ends the whole test program instead of failing
// one test function. These look at ok() first, so an unexpected success or
// failure is an ordinary FAIL line with the reason.

#include <QTest>

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
