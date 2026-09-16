# alert-engine · 告警与事件引擎（独立构建）
#
# 契约依据：include/alert_engine/alert_engine.h（唯一公开头）
# 需求依据：ALT-NFR-01（零外部依赖）/ ALT-NFR-05（一条命令构建 → 一条命令验收）
#
# 目标：
#   alert_engine      静态库（宿主唯一依赖）
#   example_*         examples/ 下的示例
#   selftest          tests/ 下的零依赖自测（注册为 ctest 用例）
#
# 依赖：仅 nlohmann/json（优先系统包，找不到时回落内置单头 third_party/nlohmann/json.hpp）。
# 本模块**永远不需要** Web 框架 / SQL / 其它引擎（protocol.md P1/P2/P3、ALT-NFR-01）。
cmake_minimum_required(VERSION 3.20)
project(alert_engine
  VERSION 0.1.0
  DESCRIPTION "告警与事件引擎：规则求值 / 产生分级 / 去重合并 / 抖动抑制 / 升级 / 抑制关系 / 风暴保护 / 确认与生命周期 / 台账查询与订阅"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# ---------------------------------------------------------------- 选项
option(ALERT_ENGINE_BUILD_TESTS    "编译 tests/（零依赖自测）" ON)
option(ALERT_ENGINE_BUILD_EXAMPLES "编译 examples/（最小示例、全流程演示）" ON)
option(ALERT_ENGINE_WARNINGS_AS_ERRORS "把编译告警当错误" OFF)

# ---------------------------------------------------------------- 依赖
find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
  set(ALERT_ENGINE_JSON_TARGET nlohmann_json::nlohmann_json)
  message(STATUS "[alert-engine] nlohmann/json: 使用已安装的包")
else()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    message(FATAL_ERROR
      "[alert-engine] 未找到 nlohmann/json：既没有安装包，也没有内置副本 "
      "third_party/nlohmann/json.hpp。请安装 nlohmann-json 或放入内置副本。")
  endif()
  add_library(alert_engine_nlohmann INTERFACE)
  target_include_directories(alert_engine_nlohmann INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  set(ALERT_ENGINE_JSON_TARGET alert_engine_nlohmann)
  message(STATUS "[alert-engine] nlohmann/json: 使用内置单头副本 third_party/nlohmann/json.hpp")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- 库
file(GLOB_RECURSE ALERT_ENGINE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")

add_library(alert_engine STATIC ${ALERT_ENGINE_SOURCES})
add_library(alert_engine::alert_engine ALIAS alert_engine)

target_include_directories(alert_engine
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(alert_engine
  PUBLIC  ${ALERT_ENGINE_JSON_TARGET}
  PRIVATE Threads::Threads)

target_compile_definitions(alert_engine PUBLIC
  ALERT_ENGINE_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(alert_engine PRIVATE /utf-8 /W4 /permissive-)
  target_compile_definitions(alert_engine PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS)
  if(ALERT_ENGINE_WARNINGS_AS_ERRORS)
    target_compile_options(alert_engine PRIVATE /WX)
  endif()
else()
  target_compile_options(alert_engine PRIVATE -Wall -Wextra)
  if(ALERT_ENGINE_WARNINGS_AS_ERRORS)
    target_compile_options(alert_engine PRIVATE -Werror)
  endif()
endif()

set_target_properties(alert_engine PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------- 示例 / 测试
if(ALERT_ENGINE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(ALERT_ENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- 汇总
message(STATUS "[alert-engine] ${PROJECT_NAME} ${PROJECT_VERSION} · C++${CMAKE_CXX_STANDARD}")
message(STATUS "[alert-engine]   构建类型 : ${CMAKE_BUILD_TYPE}")
message(STATUS "[alert-engine]   examples/tests : "
               "${ALERT_ENGINE_BUILD_EXAMPLES}/${ALERT_ENGINE_BUILD_TESTS}")
message(STATUS "[alert-engine]   规则包目录 : policies/mapapp/alertRules.json")
