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

/*
 * AutomationTest: loads the world like mangosd (same configuration files, databases and data directory, no network)
 * and runs the registered automation tests (AutomationTest.h) on the world thread between World::Update calls.
 *
 *   AutomationTest [options] [filter ...]
 *
 * Without filters every registered test runs, otherwise those whose full name contains one of the filters (case
 * insensitive), e.g. "MangosTests.Playerbots" or "Mage". Exit code: 0 all passed, 1 a test failed, 2 startup failed.
 */

#include "Common.h"
#include "AutomationTest.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "Log/Log.h"
#include "Log/LogHelper.h"
#include "Platform/Define.h"
#include "SystemConfig.h"
#include "Util/ProgressBar.h"
#include "Util/Timer.h"
#include "World/World.h"
#include "revision_sql.h"
#include "AuctionHouseBot/AuctionHouseBot.h"
#include "PlayerBot/config.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/provider.h>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef ENABLE_PLAYERBOTS
#include "playerbot/PlayerbotAIConfig.h"
#endif

DatabaseType WorldDatabase;                                 ///< Accessor to the world database
DatabaseType CharacterDatabase;                             ///< Accessor to the character database
DatabaseType LoginDatabase;                                 ///< Accessor to the realm/login database
DatabaseType LogsDatabase;                                  ///< Accessor to the logs database

uint32 realmID;                                             ///< Id of the realm

#ifdef _WIN32
int m_ServiceStatus = -1;                                   // never a service
#endif

namespace
{
    constexpr uint32 worldSleepConst = 50;                  // like WorldRunnable

    bool StartDatabase(DatabaseType& database, char const* name, char const* infoKey, char const* connectionsKey, char const* versionTable, char const* version)
    {
        std::string const info = sConfig.GetStringDefault(infoKey);
        int const connections = sConfig.GetIntDefault(connectionsKey, 1);
        if (info.empty())
        {
            sLog.outError("%s database not specified in configuration file", name);
            return false;
        }

        if (!database.Initialize(info.c_str(), connections))
        {
            sLog.outError("Cannot connect to %s database %s", name, info.c_str());
            return false;
        }

        return database.CheckRequiredField(versionTable, version);
    }

    bool StartDatabases()
    {
        bool ok = StartDatabase(WorldDatabase, "World", "WorldDatabaseInfo", "WorldDatabaseConnections", "db_version", REVISION_DB_MANGOS) &&
            StartDatabase(CharacterDatabase, "Character", "CharacterDatabaseInfo", "CharacterDatabaseConnections", "character_db_version", REVISION_DB_CHARACTERS) &&
            StartDatabase(LoginDatabase, "Login", "LoginDatabaseInfo", "LoginDatabaseConnections", "realmd_db_version", REVISION_DB_REALMD) &&
            StartDatabase(LogsDatabase, "Logs", "LogsDatabaseInfo", "LogsDatabaseConnections", "logs_db_version", REVISION_DB_LOGS);

        realmID = sConfig.GetIntDefault("RealmID", 0);
        if (ok && !realmID)
        {
            sLog.outError("Realm ID not defined in configuration file");
            ok = false;
        }

        if (ok)
            sWorld.LoadDBVersion();
        return ok;
    }

    void StopDatabases()
    {
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        LoginDatabase.HaltDelayThread();
        LogsDatabase.HaltDelayThread();
    }
}

int main(int argc, char* argv[])
{
    try
    {
        log4cxx::PropertyConfigurator::configure("logging/mangos.properties");
    }
    catch (log4cxx::helpers::Exception&)
    {
        std::cout << "logger reading configuration failure!\n";
    }

    std::string auctionBotConfig, configFile, aiPlayerBotConfig;
    std::vector<std::string> filters;

    boost::program_options::options_description desc("Allowed options");
    desc.add_options()
    ("ahbot,a", boost::program_options::value<std::string>(&auctionBotConfig), "ahbot configuration file")
    ("config,c", boost::program_options::value<std::string>(&configFile)->default_value(_MANGOSD_CONFIG), "configuration file")
#ifdef ENABLE_PLAYERBOTS
    ("aiplayerbot,p", boost::program_options::value<std::string>(&aiPlayerBotConfig)->default_value(_D_AIPLAYERBOT_CONFIG), "aiplayerbot configuration file")
    ("random-bots", "keep the random bot logins of the playerbot configuration (off by default)")
#endif
    ("list,l", "list the registered tests (matching the filters) and exit, without loading the world")
    ("tests,t", boost::program_options::value<std::vector<std::string>>(&filters)->multitoken(), "tests to run: full names or parts of them (all when none)")
    ("help,h", "prints usage");

    boost::program_options::positional_options_description positional;
    positional.add("tests", -1);

    boost::program_options::variables_map vm;
    try
    {
        boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(desc).positional(positional).run(), vm);
        boost::program_options::notify(vm);
    }
    catch (boost::program_options::error const& e)
    {
        std::cerr << "ERROR: " << e.what() << std::endl << std::endl << desc << std::endl;
        return 2;
    }

    if (vm.count("help"))
    {
        std::cout << "AutomationTest [options] [filter ...]" << std::endl << desc << std::endl;
        return 0;
    }

    FAutomationTestFramework& framework = FAutomationTestFramework::Get();

    std::vector<FAutomationTestInfo> tests;
    framework.GetTestsMatching(filters, tests);

    if (vm.count("list"))
    {
        for (FAutomationTestInfo const& test : tests)
            std::cout << test.FullTestPath << std::endl;
        std::cout << tests.size() << " test(s)" << std::endl;
        return 0;
    }

    if (tests.empty())
    {
        std::cerr << "No test matches the filters." << std::endl;
        return 2;
    }

    if (vm.count("ahbot"))
        sAuctionHouseBot.SetConfigFileName(auctionBotConfig);

#ifdef ENABLE_PLAYERBOTS
    if (vm.count("aiplayerbot"))
        _PLAYERBOT_CONFIG = aiPlayerBotConfig;
#endif

    if (!sConfig.SetSource(configFile, "Mangosd_"))
    {
        sLog.outError("Could not find configuration file %s.", configFile.c_str());
        return 2;
    }

    AutomationTestLog::SetSink([](EAutomationMessageType type, std::string const& text)
    {
        if (type == EAutomationMessageType::Error)
            sLog.outError("%s", text.c_str());
        else
            sLog.outString("%s", text.c_str());
    });

    sLog.outString("[%s Classic AutomationTest v%s] %u test(s) to run, using configuration file %s.", _PACKAGENAME, VERSION, uint32(tests.size()), configFile.c_str());

#ifdef _WIN32
    OSSL_PROVIDER_set_default_search_path(nullptr, boost::filesystem::current_path().string().c_str());
#endif
    OSSL_PROVIDER* opensslLegacy = OSSL_PROVIDER_load(nullptr, "legacy");
    OSSL_PROVIDER* opensslDefault = OSSL_PROVIDER_load(nullptr, "default");
    if (!opensslLegacy || !opensslDefault)
    {
        sLog.outError("OpenSSL3: Failed to load providers: %s", ERR_error_string(ERR_get_error(), nullptr));
        return 2;
    }

    BarGoLink::SetOutputState(sConfig.GetBoolDefault("ShowProgressBars", true));

    if (!StartDatabases())
    {
        StopDatabases();
        return 2;
    }

    ///- the world, as Master::Run
    sWorld.SetInitialWorldSettings();

    CharacterDatabase.AllowAsyncTransactions();
    WorldDatabase.AllowAsyncTransactions();
    LoginDatabase.AllowAsyncTransactions();
    LogsDatabase.AllowAsyncTransactions();

#ifdef ENABLE_PLAYERBOTS
    // only the bots the tests log in, every object near them updated
    if (!vm.count("random-bots"))
    {
        sPlayerbotAIConfig.randomBotAutologin = false;
        sPlayerbotAIConfig.minRandomBots = 0;
        sPlayerbotAIConfig.maxRandomBots = 0;
    }
    sPlayerbotAIConfig.disableBotOptimizations = true;
#endif

    ///- the world loop of WorldRunnable on this thread, the tests stepped after each update
    WorldDatabase.ThreadStart();
    sWorld.InitResultQueue();

    framework.QueueTests(tests);

    uint32 diff = 0;
    WorldTimer::tick();
    while (!World::IsStopped())
    {
        ++World::m_worldLoopCounter;

        diff = WorldTimer::tick();
        sWorld.Update(diff);

        if (!framework.Tick(diff))
            break;

        uint32 const updateTime = WorldTimer::getMSTime() - WorldTimer::tickTime();
        if (updateTime < worldSleepConst)
            std::this_thread::sleep_for(std::chrono::milliseconds(worldSleepConst - updateTime));
    }

    framework.PrintSummary();
    uint32 const failed = framework.GetFailedCount() + uint32(tests.size() - framework.GetResults().size());

    ///- shutdown, as Master::Run
    World::StopNow(SHUTDOWN_EXIT_CODE);
    sWorld.CleanupsBeforeStop();
    WorldDatabase.ThreadEnd();
    StopDatabases();

    OSSL_PROVIDER_unload(opensslLegacy);
    OSSL_PROVIDER_unload(opensslDefault);

    return failed ? 1 : 0;
}
