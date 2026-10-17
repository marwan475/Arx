#pragma once

#include "SelfTestManager.hpp"

#include <klib/klib.h>
#include <klib/printf/printf.h>

// One coherent public contract, with any number of related assertions.
// Declare owned fixtures after the case so cleanup completes before reporting.
class TestCase
{
public:
    explicit TestCase(const char* name) : Name(name), Passes(0), Failures(0), Status(SELFTEST_PASS)
    {
        SelfTestManager::Get().CaseBegin(name);
    }

    ~TestCase()
    {
        if (Status == SELFTEST_PASS && Passes == 0 && Failures == 0)
        {
            Block("no checks executed");
        }

        SelfTestManager::Get().CaseFinish(Name, Passes, Failures, Failures ? SELFTEST_FAIL : Status);
    }

    TestCase(const TestCase&) = delete;
    TestCase& operator=(const TestCase&) = delete;
    bool Check(bool condition, const char* expectation)
    {
        if (condition)
        {
            ++Passes;
        }
        else
        {
            ++Failures;
            SelfTestManager::Get().RecordFailureDetail(expectation);
        }

        return condition;
    }

    bool Equal(uint64_t actual, uint64_t expected, const char* expectation)
    {
        if (actual == expected)
        {
            ++Passes;
            return true;
        }

        ++Failures;
        char detail[256];
        snprintf(detail, sizeof(detail), "%s expected=0x%llx actual=0x%llx", expectation, (unsigned long long) expected, (unsigned long long) actual);
        SelfTestManager::Get().RecordFailureDetail(detail);
        return false;
    }

    void Skip(const char* reason)
    {
        Status = SELFTEST_SKIP;
        kprintf("[SELFTEST] SKIP %s: %s\n", Name, reason);
    }

    void Block(const char* reason)
    {
        Status = SELFTEST_BLOCKED;
        kprintf("[SELFTEST] BLOCKED %s: %s\n", Name, reason);
    }

private:
    const char*        Name;
    unsigned long long Passes;
    unsigned long long Failures;
    selftest_status_t  Status;
};
