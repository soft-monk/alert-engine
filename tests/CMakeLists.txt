# tests/ · 零依赖自测（一个可执行 + 手写断言，不引任何测试框架）
#
# 覆盖需求专篇 docs/需求/alert-engine需求专篇.md 的 35 条
# （ALT-RULE 5 / ALT-GEN 6 / ALT-DEDUP 6 / ALT-ACK 5 / ALT-SUB 6 / ALT-NFR 7）
# 与共享契约 protocol.md 的可机检清单。**不需要任何外部服务、不需要网络、不依赖真实时间**
# （时钟一律注入假时钟 ALT-NFR-02）。
#
# 运行： selftest            → 跑全部
#        selftest --list     → 只列用例名
#        selftest --json     → 机检输出（acceptance.ps1 读它做需求↔用例对账）
#        selftest <名字片段>  → 只跑名字里含该片段的用例

add_executable(selftest selftest.cc)
target_link_libraries(selftest PRIVATE alert_engine)

set_target_properties(selftest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
  # /wd4819 /wd4474：源文件是 UTF-8，但 MSVC 在控制台代码页被切到 UTF-8（见 selftest 的 chcp 65001）
  # 时会报"当前代码页(0)"—— 属环境噪声，非源码问题。
  target_compile_options(selftest PRIVATE /utf-8 /W4 /wd4819 /wd4474)
  target_compile_definitions(selftest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(selftest PRIVATE -Wall -Wextra)
endif()

# 规则包目录：自测通过编译期宏拿到，**不依赖当前工作目录**（ALT-NFR-05）
target_compile_definitions(selftest PRIVATE
  ALERT_ENGINE_POLICY_DIR="${CMAKE_SOURCE_DIR}/policies/mapapp")

add_test(NAME alert_engine_selftest COMMAND selftest)

