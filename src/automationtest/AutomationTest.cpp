/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "AutomationTest.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>

namespace
{
    std::function<void(EAutomationMessageType, std::string const&)>& GetSink()
    {
        static std::function<void(EAutomationMessageType, std::string const&)> sink;
        return sink;
    }

    std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    // runs a step of a test, an exception is an error of the test
    template<typename F>
    bool Guarded(FAutomationTestBase* test, char const* what, F&& function)
    {
        try
        {
            function();
            return true;
        }
        catch (std::exception const& e)
        {
            if (test)
                test->AddError(std::string("Exception in ") + what + ": " + e.what());
        }
        catch (...)
        {
            if (test)
                test->AddError(std::string("Unknown exception in ") + what);
        }
        return false;
    }
}

//////////////////////////////////////////////////////////////////////////
// AutomationTestLog

void AutomationTestLog::SetSink(std::function<void(EAutomationMessageType type, std::string const& text)> sink)
{
    GetSink() = std::move(sink);
}

void AutomationTestLog::Write(EAutomationMessageType type, std::string const& text)
{
    if (GetSink())
        GetSink()(type, text);
    else
        std::printf("%s\n", text.c_str());
}

//////////////////////////////////////////////////////////////////////////
// latent commands

bool IAutomationLatentCommand::InternalUpdate()
{
    if (!m_started)
    {
        m_started = true;
        m_startTime = std::chrono::steady_clock::now();
    }
    return Update();
}

uint32_t IAutomationLatentCommand::GetCurrentRunTime() const
{
    if (!m_started)
        return 0;
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_startTime).count());
}

bool FDelayedFunctionLatentCommand::Update()
{
    if (GetCurrentRunTime() < m_delayMs)
        return false;
    m_function();
    return true;
}

bool FUntilDoneLatentCommand::Update()
{
    if (!m_called)
    {
        m_called = true;
        if (!Guarded(m_test, m_description.empty() ? "latent step" : m_description.c_str(), [&]() { m_function(m_done); }))
            return true;
    }

    if (m_done.IsDone())
        return true;

    if (GetCurrentRunTime() >= m_timeoutMs)
    {
        if (m_test)
            m_test->AddError((m_description.empty() ? std::string("Latent command") : m_description) + " timed out after " + std::to_string(m_timeoutMs) + " ms.");
        return true;
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////
// FAutomationTestBase

FAutomationTestBase::FAutomationTestBase(std::string const& name, bool complexTask) : m_testName(name), m_complexTask(complexTask)
{
    FAutomationTestFramework::Get().RegisterAutomationTest(m_testName, this);
}

FAutomationTestBase::~FAutomationTestBase()
{
    FAutomationTestFramework::Get().UnregisterAutomationTest(m_testName);
}

void FAutomationTestBase::GetTests(std::vector<std::string>& beautifiedNames, std::vector<std::string>& testCommands) const
{
    beautifiedNames.push_back(std::string());
    testCommands.push_back(std::string());
}

void FAutomationTestBase::AddError(std::string const& text)
{
    AddMessage(EAutomationMessageType::Error, text);
}

void FAutomationTestBase::AddWarning(std::string const& text)
{
    AddMessage(EAutomationMessageType::Warning, text);
}

void FAutomationTestBase::AddInfo(std::string const& text)
{
    AddMessage(EAutomationMessageType::Info, text);
}

void FAutomationTestBase::AddMessage(EAutomationMessageType type, std::string const& text)
{
    if (type == EAutomationMessageType::Error)
        ++m_errorCount;
    else if (type == EAutomationMessageType::Warning)
        ++m_warningCount;
    m_messages.push_back({ type, text });

    char const* prefix = type == EAutomationMessageType::Error ? "    Error: " : type == EAutomationMessageType::Warning ? "    Warning: " : "    ";
    AutomationTestLog::Write(type, prefix + text);
}

void FAutomationTestBase::ClearExecutionInfo()
{
    m_messages.clear();
    m_errorCount = 0;
    m_warningCount = 0;
}

bool FAutomationTestBase::TestTrue(std::string const& what, bool value)
{
    if (!value)
        AddError("Expected '" + what + "' to be true.");
    return value;
}

bool FAutomationTestBase::TestEqual(std::string const& what, float actual, float expected, float tolerance)
{
    if (std::fabs(actual - expected) <= tolerance)
        return true;
    AddError("Expected '" + what + "' to be " + std::to_string(expected) + ", but it was " + std::to_string(actual) + " within tolerance " + std::to_string(tolerance) + ".");
    return false;
}

bool FAutomationTestBase::TestEqual(std::string const& what, double actual, double expected, double tolerance)
{
    if (std::fabs(actual - expected) <= tolerance)
        return true;
    AddError("Expected '" + what + "' to be " + std::to_string(expected) + ", but it was " + std::to_string(actual) + " within tolerance " + std::to_string(tolerance) + ".");
    return false;
}

void FAutomationTestBase::AddCommand(IAutomationLatentCommand* command)
{
    AddCommand(std::shared_ptr<IAutomationLatentCommand>(command));
}

void FAutomationTestBase::AddCommand(std::shared_ptr<IAutomationLatentCommand> command)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(std::move(command));
}

void FAutomationTestBase::AddTicker(std::function<bool(uint32_t diff)> ticker)
{
    FAutomationTestFramework::Get().AddTicker(std::move(ticker));
}

//////////////////////////////////////////////////////////////////////////
// FAutomationSpecBase

void FAutomationSpecBase::GetTests(std::vector<std::string>& beautifiedNames, std::vector<std::string>& testCommands) const
{
    EnsureDefinitions();
    for (FSpec const& spec : m_specs)
    {
        beautifiedNames.push_back(spec.Description);
        testCommands.push_back(spec.Id);
    }
}

bool FAutomationSpecBase::RunTest(std::string const& parameters)
{
    EnsureDefinitions();

    auto itr = m_specsById.find(parameters);
    if (itr == m_specsById.end())
    {
        AddError("No spec '" + parameters + "' in " + GetTestName() + ".");
        return false;
    }

    // a fresh latent command per step, run in order by the framework
    for (FStepFactory const& factory : m_specs[itr->second].Commands)
        AddCommand(factory());
    return true;
}

void FAutomationSpecBase::Describe(std::string const& description, std::function<void()> doWork)
{
    FSpecDefinitionScope* parent = m_scopeStack.back();
    parent->Children.push_back(std::make_unique<FSpecDefinitionScope>());
    FSpecDefinitionScope* scope = parent->Children.back().get();
    scope->Description = description;

    m_scopeStack.push_back(scope);
    doWork();
    m_scopeStack.pop_back();
}

void FAutomationSpecBase::It(std::string const& description, std::function<void()> doWork)
{
    m_scopeStack.back()->It.emplace_back(description, MakeStep(std::move(doWork)));
}

void FAutomationSpecBase::It(std::string const& description, uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork)
{
    m_scopeStack.back()->It.emplace_back(description, MakeLatentStep(timeoutMs, std::move(doWork), "It '" + description + "'"));
}

void FAutomationSpecBase::BeforeEach(std::function<void()> doWork)
{
    m_scopeStack.back()->BeforeEach.push_back(MakeStep(std::move(doWork)));
}

void FAutomationSpecBase::BeforeEach(uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork)
{
    m_scopeStack.back()->BeforeEach.push_back(MakeLatentStep(timeoutMs, std::move(doWork), "BeforeEach"));
}

void FAutomationSpecBase::AfterEach(std::function<void()> doWork)
{
    m_scopeStack.back()->AfterEach.push_back(MakeStep(std::move(doWork)));
}

void FAutomationSpecBase::AfterEach(uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork)
{
    m_scopeStack.back()->AfterEach.push_back(MakeLatentStep(timeoutMs, std::move(doWork), "AfterEach"));
}

FAutomationSpecBase::FStepFactory FAutomationSpecBase::MakeStep(std::function<void()> doWork)
{
    return [this, doWork]()
    {
        return std::shared_ptr<IAutomationLatentCommand>(std::make_shared<FFunctionLatentCommand>([this, doWork]()
        {
            Guarded(this, "spec step", doWork);
            return true;
        }));
    };
}

FAutomationSpecBase::FStepFactory FAutomationSpecBase::MakeLatentStep(uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork, std::string description)
{
    return [this, timeoutMs, doWork, description]()
    {
        return std::shared_ptr<IAutomationLatentCommand>(std::make_shared<FUntilDoneLatentCommand>(this, doWork, timeoutMs, description));
    };
}

void FAutomationSpecBase::EnsureDefinitions() const
{
    if (m_defined)
        return;

    // definitions are built once, on first use (after static initialization, like Unreal)
    FAutomationSpecBase* self = const_cast<FAutomationSpecBase*>(this);
    self->m_defined = true;
    self->m_rootScope = std::make_unique<FSpecDefinitionScope>();
    self->m_scopeStack.push_back(self->m_rootScope.get());
    if (!Guarded(self, "Define", [self]() { self->Define(); }))
        AutomationTestLog::Write(EAutomationMessageType::Error, "Define of " + GetTestName() + " failed.");
    self->m_scopeStack.clear();
    self->PostDefine();
}

void FAutomationSpecBase::PostDefine()
{
    std::vector<FSpecDefinitionScope const*> path;
    Flatten(*m_rootScope, path);
}

void FAutomationSpecBase::Flatten(FSpecDefinitionScope const& scope, std::vector<FSpecDefinitionScope const*>& path)
{
    path.push_back(&scope);

    for (auto const& it : scope.It)
    {
        FSpec spec;

        std::string description;
        for (FSpecDefinitionScope const* itr : path)
            if (!itr->Description.empty())
                description += itr->Description + " ";
        description += it.first;
        spec.Description = description;

        // ids stay unique when two Its have the same text
        spec.Id = description;
        for (uint32_t suffix = 2; m_specsById.count(spec.Id); ++suffix)
            spec.Id = description + " (" + std::to_string(suffix) + ")";
        spec.Description = spec.Id;

        // BeforeEach outer to inner, the It, AfterEach inner to outer
        for (FSpecDefinitionScope const* itr : path)
            spec.Commands.insert(spec.Commands.end(), itr->BeforeEach.begin(), itr->BeforeEach.end());
        spec.Commands.push_back(it.second);
        for (auto itr = path.rbegin(); itr != path.rend(); ++itr)
            spec.Commands.insert(spec.Commands.end(), (*itr)->AfterEach.begin(), (*itr)->AfterEach.end());

        m_specsById[spec.Id] = m_specs.size();
        m_specs.push_back(std::move(spec));
    }

    for (auto const& child : scope.Children)
        Flatten(*child, path);

    path.pop_back();
}

//////////////////////////////////////////////////////////////////////////
// FAutomationTestFramework

FAutomationTestFramework& FAutomationTestFramework::Get()
{
    static FAutomationTestFramework framework;
    return framework;
}

bool FAutomationTestFramework::RegisterAutomationTest(std::string const& testName, FAutomationTestBase* test)
{
    if (m_tests.count(testName))
    {
        AutomationTestLog::Write(EAutomationMessageType::Error, "Automation test '" + testName + "' is registered twice, the second one is ignored.");
        return false;
    }
    m_tests[testName] = test;
    return true;
}

bool FAutomationTestFramework::UnregisterAutomationTest(std::string const& testName)
{
    return m_tests.erase(testName) > 0;
}

void FAutomationTestFramework::GetValidTestNames(std::vector<FAutomationTestInfo>& tests) const
{
    for (auto const& itr : m_tests)
    {
        std::vector<std::string> beautifiedNames, testCommands;
        itr.second->GetTests(beautifiedNames, testCommands);
        for (size_t i = 0; i < beautifiedNames.size() && i < testCommands.size(); ++i)
        {
            FAutomationTestInfo info;
            info.TestName = itr.first;
            info.FullTestPath = beautifiedNames[i].empty() ? itr.first : itr.first + "." + beautifiedNames[i];
            info.TestCommand = testCommands[i];
            info.TestFlags = itr.second->GetTestFlags();
            info.Test = itr.second;
            tests.push_back(std::move(info));
        }
    }

    std::sort(tests.begin(), tests.end(), [](FAutomationTestInfo const& a, FAutomationTestInfo const& b) { return a.FullTestPath < b.FullTestPath; });
}

void FAutomationTestFramework::GetTestsMatching(std::vector<std::string> const& filters, std::vector<FAutomationTestInfo>& tests) const
{
    std::vector<FAutomationTestInfo> all;
    GetValidTestNames(all);

    std::vector<std::string> lowerFilters;
    for (std::string const& filter : filters)
        if (!filter.empty())
            lowerFilters.push_back(ToLower(filter));

    for (FAutomationTestInfo& info : all)
    {
        std::string const path = ToLower(info.FullTestPath);
        bool const disabled = (info.TestFlags & EAutomationTestFlags::Disabled) != 0;

        bool matches = lowerFilters.empty() && !disabled;
        for (std::string const& filter : lowerFilters)
        {
            if (disabled ? path == filter : path.find(filter) != std::string::npos)
            {
                matches = true;
                break;
            }
        }

        if (matches)
            tests.push_back(std::move(info));
    }
}

void FAutomationTestFramework::QueueTests(std::vector<FAutomationTestInfo> const& tests)
{
    m_queue.insert(m_queue.end(), tests.begin(), tests.end());
}

bool FAutomationTestFramework::Tick(uint32_t diff)
{
    if (!m_current)
    {
        if (m_queue.empty())
            return false;

        FAutomationTestInfo test = m_queue.front();
        m_queue.pop_front();
        StartTest(test);
    }
    else
        ExecuteTickers(diff);

    ExecuteLatentCommands();

    if (m_latentCommands.empty())
        FinishTest();

    return IsRunning();
}

void FAutomationTestFramework::StartTest(FAutomationTestInfo const& test)
{
    m_current = std::make_unique<FAutomationTestInfo>(test);
    m_currentTest = test.Test;
    m_currentStart = std::chrono::steady_clock::now();
    m_latentCommands.clear();
    m_tickers.clear();

    AutomationTestLog::Write(EAutomationMessageType::Info, "Test Started. Name={" + test.FullTestPath + "}");

    m_currentTest->ClearExecutionInfo();
    bool started = false;
    Guarded(m_currentTest, "RunTest", [&]() { started = m_currentTest->RunTest(test.TestCommand); });
    m_currentStartedOk = started;
}

void FAutomationTestFramework::FinishTest()
{
    if (!m_current)
        return;

    FResult result;
    result.FullTestPath = m_current->FullTestPath;
    result.Errors = m_currentTest->GetErrorCount();
    result.Warnings = m_currentTest->GetWarningCount();
    result.Success = m_currentStartedOk && !m_currentTest->HasAnyErrors();
    result.DurationMs = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_currentStart).count());
    m_results.push_back(result);

    AutomationTestLog::Write(result.Success ? EAutomationMessageType::Info : EAutomationMessageType::Error,
        std::string("Test Completed. Result={") + (result.Success ? "Success" : "Fail") + "} Name={" + result.FullTestPath + "} Duration={" +
        std::to_string(result.DurationMs) + " ms} Errors={" + std::to_string(result.Errors) + "} Warnings={" + std::to_string(result.Warnings) + "}");

    m_tickers.clear();
    m_latentCommands.clear();
    m_current.reset();
    m_currentTest = nullptr;
}

void FAutomationTestFramework::ExecuteLatentCommands()
{
    // as many as end within this tick, like Unreal
    while (!m_latentCommands.empty())
    {
        std::shared_ptr<IAutomationLatentCommand> command = m_latentCommands.front();
        bool done = true;
        Guarded(m_currentTest, "latent command", [&]() { done = command->InternalUpdate(); });
        if (!done)
            break;
        m_latentCommands.pop_front();
    }
}

void FAutomationTestFramework::ExecuteTickers(uint32_t diff)
{
    // tickers added meanwhile run from the next tick
    std::vector<std::function<bool(uint32_t)>> tickers;
    tickers.swap(m_tickers);
    for (auto& ticker : tickers)
    {
        bool keep = false;
        Guarded(m_currentTest, "ticker", [&]() { keep = ticker(diff); });
        if (keep)
            m_tickers.push_back(std::move(ticker));
    }
}

void FAutomationTestFramework::EnqueueLatentCommand(std::shared_ptr<IAutomationLatentCommand> command)
{
    m_latentCommands.push_back(std::move(command));
}

void FAutomationTestFramework::AddTicker(std::function<bool(uint32_t diff)> ticker)
{
    m_tickers.push_back(std::move(ticker));
}

uint32_t FAutomationTestFramework::GetFailedCount() const
{
    return uint32_t(std::count_if(m_results.begin(), m_results.end(), [](FResult const& result) { return !result.Success; }));
}

void FAutomationTestFramework::PrintSummary() const
{
    AutomationTestLog::Write(EAutomationMessageType::Info, "==================== Automation test results ====================");
    for (FResult const& result : m_results)
        AutomationTestLog::Write(result.Success ? EAutomationMessageType::Info : EAutomationMessageType::Error,
            std::string(result.Success ? "  [ OK ] " : "  [FAIL] ") + result.FullTestPath + " (" + std::to_string(result.DurationMs) + " ms)");
    AutomationTestLog::Write(EAutomationMessageType::Info, std::to_string(m_results.size() - GetFailedCount()) + " passed, " + std::to_string(GetFailedCount()) + " failed, " +
        std::to_string(m_results.size()) + " total");
}
