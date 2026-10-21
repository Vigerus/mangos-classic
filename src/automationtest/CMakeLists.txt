#
# This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#

# AutomationTest: runs the registered automation tests (AutomationTest.h) in a world loaded like mangosd's.
# Tests of other targets are object libraries linked in here (static library objects would be dropped by the
# linker, their tests register by static instances): playerbots-tests from src/modules/PlayerBots/Tests.

set(EXECUTABLE_NAME AutomationTest)

set(EXECUTABLE_SRCS
    AutomationTest.cpp
    AutomationTest.h
    Main.cpp
    TestWorld.cpp
    TestWorld.h
    # the console commands of the chat command table (ChatHandler::HandleServerExitCommand, ...) live in mangosd
    ${CMAKE_SOURCE_DIR}/src/mangosd/CliRunnable.cpp
    ${CMAKE_SOURCE_DIR}/src/mangosd/CliRunnable.h
   )

add_executable(${EXECUTABLE_NAME}
  ${EXECUTABLE_SRCS}
)

target_include_directories(${EXECUTABLE_NAME}
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${EXECUTABLE_NAME}
  shared
  game
  cmangos-compile-option-interface
)

if(TARGET playerbots-tests)
  target_link_libraries(${EXECUTABLE_NAME} playerbots-tests)
endif()

if(WIN32)
  if(MSVC)
    target_link_libraries(${EXECUTABLE_NAME}
      dbghelp
      winmm.lib
    )
  endif()
  if(MINGW)
    target_link_libraries(${EXECUTABLE_NAME}
      wsock32
      ws2_32
    )
  endif()

  # built next to mangosd; Visual Studio runs it in the server directory that feeds the data (mangosd.conf,
  # aiplayerbot.conf, dbc, maps, vmaps, mmaps, logging), e.g. -DAUTOMATIONTEST_WORKING_DIR=F:/server/vanillanew
  set(AUTOMATIONTEST_WORKING_DIR "" CACHE PATH "Server directory the AutomationTest debugger runs in (configuration and data files), the binary directory when empty")
  if(AUTOMATIONTEST_WORKING_DIR)
    set(AUTOMATIONTEST_DEBUGGER_DIR "${AUTOMATIONTEST_WORKING_DIR}")
  else()
    set(AUTOMATIONTEST_DEBUGGER_DIR "${DEV_BIN_DIR}")
  endif()
  set_target_properties(${EXECUTABLE_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DEV_BIN_DIR}")
  set_target_properties(${EXECUTABLE_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${AUTOMATIONTEST_DEBUGGER_DIR}")
  set_target_properties(${EXECUTABLE_NAME} PROPERTIES PROJECT_LABEL "AutomationTest")
endif()

set(EXECUTABLE_LINK_FLAGS "")

if(UNIX)
  if (APPLE)
    set(EXECUTABLE_LINK_FLAGS "-pthread -framework Carbon")
  else()
    set(EXECUTABLE_LINK_FLAGS "-pthread -rdynamic")
  endif()
endif()

set_target_properties(${EXECUTABLE_NAME} PROPERTIES LINK_FLAGS
  "${EXECUTABLE_LINK_FLAGS}"
)

install(TARGETS ${EXECUTABLE_NAME} DESTINATION ${BIN_DIR})

# Define BUILD_DEPRECATED_PLAYERBOT if need
if (BUILD_DEPRECATED_PLAYERBOT)
  add_definitions(-DBUILD_DEPRECATED_PLAYERBOT)
endif()

# Define ENABLE_PLAYERBOTS if need
if (BUILD_PLAYERBOTS)
  add_definitions(-DENABLE_PLAYERBOTS)
endif()

if(MSVC)
  install(FILES $<TARGET_PDB_FILE:${EXECUTABLE_NAME}> DESTINATION ${BIN_DIR} CONFIGURATIONS Debug)
endif()
