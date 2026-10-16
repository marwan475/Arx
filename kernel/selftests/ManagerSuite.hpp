#pragma once

#include "TestCase.hpp"

class ManagerSuite
{
public:
    enum class CapabilityReport
    {
        SeparateCase,
        InContract
    };

    ManagerSuite(const char* name, bool available, CapabilityReport report = CapabilityReport::SeparateCase) : Name(name), Available(available), Report(report)
    {
        SelfTestManager::Get().GroupBegin(Name);

        if (Report == CapabilityReport::SeparateCase)
        {
            TestCase initialized(Name);

            initialized.Check(Available, "initialized manager capability is present");
        }
    }

    ~ManagerSuite()
    {
        SelfTestManager::Get().GroupEnd(Name);
    }

    template <class Function>
    void Run(const char* name, Function function)
    {
        TestCase test(name);

        if (Report == CapabilityReport::InContract)
        {
            if (!test.Check(Available, "initialized manager capability is present"))
            {
                return;
            }
        }

        if (!Available)
        {
            test.Block("manager capability unavailable");

            return;
        }

        function(test);
    }

    ManagerSuite(const ManagerSuite&) = delete;

    ManagerSuite& operator=(const ManagerSuite&) = delete;

private:
    const char*      Name;
    bool             Available;
    CapabilityReport Report;
};
