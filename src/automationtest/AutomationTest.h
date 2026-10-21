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

#ifndef MANGOS_AUTOMATIONTEST_H
#define MANGOS_AUTOMATIONTEST_H

/*
 * Automation tests in the style of Unreal's Misc/AutomationTest.h, run by the AutomationTest executable inside a
 * fully loaded world (databases, DBC, scripts, playerbots) that it ticks like mangosd does.
 *
 * Every test registers itself by a static instance (the macros below). The runner runs all of them, or the ones
 * whose full name contains one of the filters given on its command line. Test code runs on the world thread
 * between two World::Update calls, so it may use maps, objects and sessions freely.
 *
 * Simple test:
 *
 *   IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMyTest, "MangosTests.Core.Something", EAutomationTestFlags::EngineFilter)
 *   bool FMyTest::RunTest(std::string const& Parameters)
 *   {
 *       TestEqual("two", 1 + 1, 2);
 *       ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1000));    // runs over the following world ticks
 *       return true;
 *   }
 *
 * Spec (BDD style, each It is a test of its own named "<spec name>.<describes> <it>"):
 *
 *   BEGIN_DEFINE_SPEC(FMySpec, "MangosTests.Core.MySpec")
 *       uint32 Timeout = 5000;
 *   END_DEFINE_SPEC(FMySpec)
 *
 *   void FMySpec::Define()
 *   {
 *       BeforeEach([this]() { ... });
 *       Describe("a thing", [this]()
 *       {
 *           It("works", [this]() { TestTrue("works", true); });
 *           It("works later", Timeout, [this](FDoneDelegate const& Done) { ...; Done.Execute(); });
 *       });
 *       AfterEach([this]() { ... });
 *   }
 *
 * Latent steps (BeforeEach / It / AfterEach taking a timeout and a FDoneDelegate) run until Done.Execute() is
 * called, over as many world ticks as needed; not called within the timeout is an error. AddTicker runs a function
 * every world tick while the current test runs.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace EAutomationTestFlags
{
    enum Type : uint32_t
    {
        None            = 0x00000000,
        SmokeFilter     = 0x00000001,   // fast, always safe to run
        EngineFilter    = 0x00000002,   // core (game library) features
        ProductFilter   = 0x00000004,   // gameplay / module features (default)
        PerfFilter      = 0x00000008,   // performance measurements
        StressFilter    = 0x00000010,   // long running
        Disabled        = 0x80000000,   // registered but only run when named explicitly
    };

    // the flags given to BEGIN_DEFINE_SPEC / DEFINE_SPEC (optional argument), ProductFilter when none
    inline uint32_t OrDefault(std::initializer_list<uint32_t> flags)
    {
        uint32_t result = 0;
        for (uint32_t flag : flags)
            result |= flag;
        return flags.size() ? result : uint32_t(ProductFilter);
    }
}

enum class EAutomationMessageType
{
    Info,
    Warning,
    Error,
};

struct FAutomationMessage
{
    EAutomationMessageType Type;
    std::string Text;
};

// signals the end of a latent step; copies share the state
class FDoneDelegate
{
    public:
        FDoneDelegate() : m_done(std::make_shared<std::atomic<bool>>(false)) {}

        void Execute() const { m_done->store(true); }
        void ExecuteIfBound() const { Execute(); }
        bool IsBound() const { return true; }
        bool IsDone() const { return m_done->load(); }

    private:
        std::shared_ptr<std::atomic<bool>> m_done;
};

// a step run by the framework over the world ticks after RunTest returned, in order
class IAutomationLatentCommand
{
    public:
        virtual ~IAutomationLatentCommand() = default;

        // true when done, called once per world tick (and again in the same tick after the previous command ended)
        virtual bool Update() = 0;

        // first Update sets the start
        bool InternalUpdate();
        // ms since the first Update
        uint32_t GetCurrentRunTime() const;

    private:
        bool m_started = false;
        std::chrono::steady_clock::time_point m_startTime;
};

class FAutomationTestBase;

// runs a function once
class FFunctionLatentCommand : public IAutomationLatentCommand
{
    public:
        explicit FFunctionLatentCommand(std::function<bool()> function) : m_function(std::move(function)) {}
        bool Update() override { return m_function(); }

    private:
        std::function<bool()> m_function;
};

// waits the given ms
class FWaitLatentCommand : public IAutomationLatentCommand
{
    public:
        explicit FWaitLatentCommand(uint32_t durationMs) : m_durationMs(durationMs) {}
        bool Update() override { return GetCurrentRunTime() >= m_durationMs; }

    private:
        uint32_t m_durationMs;
};

// runs a function after the given ms
class FDelayedFunctionLatentCommand : public IAutomationLatentCommand
{
    public:
        FDelayedFunctionLatentCommand(std::function<void()> function, uint32_t delayMs) : m_function(std::move(function)), m_delayMs(delayMs) {}
        bool Update() override;

    private:
        std::function<void()> m_function;
        uint32_t m_delayMs;
};

// runs a function once, then waits until it calls the done delegate; an error on the test after the timeout
class FUntilDoneLatentCommand : public IAutomationLatentCommand
{
    public:
        FUntilDoneLatentCommand(FAutomationTestBase* test, std::function<void(FDoneDelegate const&)> function, uint32_t timeoutMs, std::string description = std::string())
            : m_test(test), m_function(std::move(function)), m_timeoutMs(timeoutMs), m_description(std::move(description)) {}
        bool Update() override;

    private:
        FAutomationTestBase* m_test;
        std::function<void(FDoneDelegate const&)> m_function;
        uint32_t m_timeoutMs;
        std::string m_description;
        FDoneDelegate m_done;
        bool m_called = false;
};

// a test as listed by the runner: one per (test, test command)
struct FAutomationTestInfo
{
    std::string FullTestPath;       // "<test name>[.<beautified name>]", what the filters match
    std::string TestName;
    std::string TestCommand;        // RunTest parameter
    uint32_t TestFlags = 0;
    FAutomationTestBase* Test = nullptr;
};

class FAutomationTestBase
{
    public:
        // registers the test (static instances register at load time)
        FAutomationTestBase(std::string const& name, bool complexTask);
        virtual ~FAutomationTestBase();

        FAutomationTestBase(FAutomationTestBase const&) = delete;
        FAutomationTestBase& operator=(FAutomationTestBase const&) = delete;

        std::string const& GetTestName() const { return m_testName; }
        bool IsComplexTask() const { return m_complexTask; }

        virtual uint32_t GetTestFlags() const = 0;
        virtual std::string GetTestSourceFileName() const { return std::string(); }
        virtual int32_t GetTestSourceFileLine() const { return 0; }

        // (beautified name, command) of every test it runs, one by default ("" / "")
        virtual void GetTests(std::vector<std::string>& beautifiedNames, std::vector<std::string>& testCommands) const;

        // starts the test; latent commands it adds run afterwards. False is a failure.
        virtual bool RunTest(std::string const& parameters) = 0;

        // messages of the current run, also printed at once
        void AddError(std::string const& text);
        void AddWarning(std::string const& text);
        void AddInfo(std::string const& text);
        void AddMessage(EAutomationMessageType type, std::string const& text);
        bool HasAnyErrors() const { return m_errorCount > 0; }
        uint32_t GetErrorCount() const { return m_errorCount; }
        uint32_t GetWarningCount() const { return m_warningCount; }
        std::vector<FAutomationMessage> const& GetMessages() const { return m_messages; }
        void ClearExecutionInfo();

        // checks: an error with the description when they fail, the result returned
        bool TestTrue(std::string const& what, bool value);
        bool TestFalse(std::string const& what, bool value) { return TestTrue(what, !value); }
        template<typename T> bool TestNotNull(std::string const& what, T const* pointer) { return TestTrue(what + " is not null", pointer != nullptr); }
        template<typename T> bool TestNull(std::string const& what, T const* pointer) { return TestTrue(what + " is null", pointer == nullptr); }
        bool TestEqual(std::string const& what, float actual, float expected, float tolerance = 1.e-4f);
        bool TestEqual(std::string const& what, double actual, double expected, double tolerance = 1.e-8);
        bool TestEqual(std::string const& what, char const* actual, char const* expected) { return TestEqual(what, std::string(actual ? actual : ""), std::string(expected ? expected : "")); }

        template<typename TActual, typename TExpected>
        bool TestEqual(std::string const& what, TActual const& actual, TExpected const& expected)
        {
            if (actual == expected)
                return true;
            AddError("Expected '" + what + "' to be " + ToText(expected) + ", but it was " + ToText(actual) + ".");
            return false;
        }

        template<typename TActual, typename TExpected>
        bool TestNotEqual(std::string const& what, TActual const& actual, TExpected const& expected)
        {
            if (!(actual == expected))
                return true;
            AddError("Expected '" + what + "' to differ from " + ToText(expected) + ".");
            return false;
        }

        // the given command runs after RunTest returned (or after the commands added before it)
        static void AddCommand(IAutomationLatentCommand* command);
        static void AddCommand(std::shared_ptr<IAutomationLatentCommand> command);

        // runs every world tick (diff in ms) while the current test runs, until it returns false
        static void AddTicker(std::function<bool(uint32_t diff)> ticker);

    protected:
        template<typename T>
        static std::string ToText(T const& value)
        {
            if constexpr (std::is_same_v<T, std::string>)
                return "'" + value + "'";
            else if constexpr (std::is_enum_v<T>)
                return std::to_string(static_cast<std::underlying_type_t<T>>(value));
            else if constexpr (std::is_arithmetic_v<T>)
            {
                std::ostringstream out;
                out << +value;
                return out.str();
            }
            else
                return "<value>";
        }

    private:
        std::string m_testName;
        bool m_complexTask;
        std::vector<FAutomationMessage> m_messages;
        uint32_t m_errorCount = 0;
        uint32_t m_warningCount = 0;
};

/*
 * Base of the specs (BEGIN_DEFINE_SPEC / DEFINE_SPEC): Define() describes the tests with Describe / It /
 * BeforeEach / AfterEach once, the first time the spec is listed or run.
 */
class FAutomationSpecBase : public FAutomationTestBase
{
    public:
        FAutomationSpecBase(std::string const& name, bool complexTask) : FAutomationTestBase(name, complexTask) {}

        void GetTests(std::vector<std::string>& beautifiedNames, std::vector<std::string>& testCommands) const override;
        bool RunTest(std::string const& parameters) override;

    protected:
        virtual void Define() = 0;

        // default timeout of the latent steps given without one
        uint32_t DefaultTimeout = 30000;

        void Describe(std::string const& description, std::function<void()> doWork);
        void xDescribe(std::string const& /*description*/, std::function<void()> /*doWork*/) {}

        void It(std::string const& description, std::function<void()> doWork);
        void It(std::string const& description, uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork);
        void LatentIt(std::string const& description, std::function<void(FDoneDelegate const&)> doWork) { It(description, DefaultTimeout, std::move(doWork)); }
        void LatentIt(std::string const& description, uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork) { It(description, timeoutMs, std::move(doWork)); }
        void xIt(std::string const& /*description*/, std::function<void()> /*doWork*/) {}
        void xIt(std::string const& /*description*/, uint32_t /*timeoutMs*/, std::function<void(FDoneDelegate const&)> /*doWork*/) {}

        void BeforeEach(std::function<void()> doWork);
        void BeforeEach(uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork);
        void LatentBeforeEach(std::function<void(FDoneDelegate const&)> doWork) { BeforeEach(DefaultTimeout, std::move(doWork)); }
        void LatentBeforeEach(uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork) { BeforeEach(timeoutMs, std::move(doWork)); }

        void AfterEach(std::function<void()> doWork);
        void AfterEach(uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork);
        void LatentAfterEach(std::function<void(FDoneDelegate const&)> doWork) { AfterEach(DefaultTimeout, std::move(doWork)); }
        void LatentAfterEach(uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork) { AfterEach(timeoutMs, std::move(doWork)); }

    private:
        // makes the latent command of a step for one run
        using FStepFactory = std::function<std::shared_ptr<IAutomationLatentCommand>()>;

        struct FSpecDefinitionScope
        {
            std::string Description;
            std::vector<FStepFactory> BeforeEach;
            std::vector<FStepFactory> AfterEach;
            std::vector<std::pair<std::string, FStepFactory>> It;
            std::vector<std::unique_ptr<FSpecDefinitionScope>> Children;
        };

        // one It with the BeforeEach / AfterEach of its scopes
        struct FSpec
        {
            std::string Id;
            std::string Description;
            std::vector<FStepFactory> Commands;
        };

        FStepFactory MakeStep(std::function<void()> doWork);
        FStepFactory MakeLatentStep(uint32_t timeoutMs, std::function<void(FDoneDelegate const&)> doWork, std::string description);

        void EnsureDefinitions() const;
        void PostDefine();
        void Flatten(FSpecDefinitionScope const& scope, std::vector<FSpecDefinitionScope const*>& path);

        std::unique_ptr<FSpecDefinitionScope> m_rootScope;
        std::vector<FSpecDefinitionScope*> m_scopeStack;
        std::vector<FSpec> m_specs;
        std::map<std::string, size_t> m_specsById;
        bool m_defined = false;
};

/*
 * The registry and the runner. The executable queues the tests, then calls Tick after every World::Update until it
 * returns false.
 */
class FAutomationTestFramework
{
    public:
        static FAutomationTestFramework& Get();

        bool RegisterAutomationTest(std::string const& testName, FAutomationTestBase* test);
        bool UnregisterAutomationTest(std::string const& testName);

        // every test (one per test command), sorted by full path
        void GetValidTestNames(std::vector<FAutomationTestInfo>& tests) const;
        // the tests whose full path contains one of the filters (case insensitive), all when none; the tests
        // flagged Disabled only when a filter is their full path
        void GetTestsMatching(std::vector<std::string> const& filters, std::vector<FAutomationTestInfo>& tests) const;

        void QueueTests(std::vector<FAutomationTestInfo> const& tests);
        // runs the queued tests a step further, call once per world tick; false when all are done
        bool Tick(uint32_t diff);
        bool IsRunning() const { return m_current != nullptr || !m_queue.empty(); }

        // the current test (nullptr between tests)
        FAutomationTestBase* GetCurrentTest() const { return m_currentTest; }

        void EnqueueLatentCommand(std::shared_ptr<IAutomationLatentCommand> command);
        void AddTicker(std::function<bool(uint32_t diff)> ticker);

        struct FResult
        {
            std::string FullTestPath;
            bool Success = false;
            uint32_t Errors = 0;
            uint32_t Warnings = 0;
            uint32_t DurationMs = 0;
        };
        std::vector<FResult> const& GetResults() const { return m_results; }
        uint32_t GetFailedCount() const;
        // the results as lines (also given to the log)
        void PrintSummary() const;

    private:
        FAutomationTestFramework() = default;

        void StartTest(FAutomationTestInfo const& test);
        void FinishTest();
        void ExecuteLatentCommands();
        void ExecuteTickers(uint32_t diff);

        std::map<std::string, FAutomationTestBase*> m_tests;

        std::deque<FAutomationTestInfo> m_queue;
        std::unique_ptr<FAutomationTestInfo> m_current;
        FAutomationTestBase* m_currentTest = nullptr;
        bool m_currentStartedOk = true;
        std::chrono::steady_clock::time_point m_currentStart;
        std::deque<std::shared_ptr<IAutomationLatentCommand>> m_latentCommands;
        std::vector<std::function<bool(uint32_t)>> m_tickers;
        std::vector<FResult> m_results;
};

// the output of the framework (log + console), set by the executable
namespace AutomationTestLog
{
    void SetSink(std::function<void(EAutomationMessageType type, std::string const& text)> sink);
    void Write(EAutomationMessageType type, std::string const& text);
}

#define ADD_LATENT_AUTOMATION_COMMAND(ClassDeclaration) FAutomationTestBase::AddCommand(new ClassDeclaration)

#define IMPLEMENT_AUTOMATION_TEST_PRIVATE(TClass, PrettyName, TFlags, bComplex) \
    class TClass : public FAutomationTestBase \
    { \
        public: \
            TClass(std::string const& name) : FAutomationTestBase(name, bComplex) {} \
            uint32_t GetTestFlags() const override { return TFlags; } \
            std::string GetTestSourceFileName() const override { return __FILE__; } \
            int32_t GetTestSourceFileLine() const override { return __LINE__; } \
            bool RunTest(std::string const& Parameters) override; \
            void GetTests(std::vector<std::string>& beautifiedNames, std::vector<std::string>& testCommands) const override; \
    };

// a test run once; define bool TClass::RunTest(std::string const& Parameters)
#define IMPLEMENT_SIMPLE_AUTOMATION_TEST(TClass, PrettyName, TFlags) \
    IMPLEMENT_AUTOMATION_TEST_PRIVATE(TClass, PrettyName, TFlags, false) \
    void TClass::GetTests(std::vector<std::string>& beautifiedNames, std::vector<std::string>& testCommands) const { FAutomationTestBase::GetTests(beautifiedNames, testCommands); } \
    namespace { TClass TClass##AutomationTestInstance(PrettyName); }

// a test run once per command of its GetTests; define bool TClass::RunTest(std::string const& Parameters) and
// void TClass::GetTests(std::vector<std::string>& beautifiedNames, std::vector<std::string>& testCommands) const
#define IMPLEMENT_COMPLEX_AUTOMATION_TEST(TClass, PrettyName, TFlags) \
    IMPLEMENT_AUTOMATION_TEST_PRIVATE(TClass, PrettyName, TFlags, true) \
    namespace { TClass TClass##AutomationTestInstance(PrettyName); }

// a spec with members declared between BEGIN_DEFINE_SPEC and END_DEFINE_SPEC; define void TClass::Define().
// The optional last argument are the EAutomationTestFlags (ProductFilter when left out).
#define BEGIN_DEFINE_SPEC(TClass, PrettyName, ...) \
    class TClass : public FAutomationSpecBase \
    { \
        public: \
            TClass(std::string const& name) : FAutomationSpecBase(name, false) {} \
            uint32_t GetTestFlags() const override { return EAutomationTestFlags::OrDefault({ __VA_ARGS__ }); } \
            std::string GetTestSourceFileName() const override { return __FILE__; } \
            int32_t GetTestSourceFileLine() const override { return __LINE__; } \
            static char const* GetPrettyName() { return PrettyName; } \
        protected: \
            void Define() override;

#define END_DEFINE_SPEC(TClass) \
    }; \
    namespace { TClass TClass##AutomationSpecInstance(TClass::GetPrettyName()); }

// a spec without members; define void TClass::Define()
#define DEFINE_SPEC(TClass, PrettyName, ...) \
    BEGIN_DEFINE_SPEC(TClass, PrettyName, __VA_ARGS__) \
    END_DEFINE_SPEC(TClass)

#endif
