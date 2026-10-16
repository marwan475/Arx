#pragma once

#include "TestCase.hpp"

class ManagerSuite
{
public:
    ManagerSuite(const char* name, bool available) : Name(name), Available(available)
    {
        SelfTestManager::Get().GroupBegin(Name);
        TestCase initialized(Name);
        initialized.Check(Available, "initialized manager capability is present");
    }

    ~ManagerSuite()
    {
        SelfTestManager::Get().GroupEnd(Name);
    }

    template <class Function> void Run(const char* name, Function function)
    {
        TestCase test(name);

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
    const char* Name;
    bool        Available;
};
