# examples/ · 两个独立可运行示例（宿主装配形态的最小与完整示范）
#
# 示例与测试一样**自己提供依赖实现**（内存 Store / 假时钟 / 打印 Sink），
# 引擎侧只认识反向接口（ALT-SUB-04 / ALT-NFR-01）。

add_executable(example_minimal minimal/main.cc)
target_link_libraries(example_minimal PRIVATE alert_engine)

add_executable(example_full_flow full_flow/main.cc)
target_link_libraries(example_full_flow PRIVATE alert_engine)

set_target_properties(example_minimal example_full_flow PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
  foreach(t example_minimal example_full_flow)
    target_compile_options(${t} PRIVATE /utf-8 /W4 /wd4819 /wd4474)
    target_compile_definitions(${t} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endforeach()
else()
  foreach(t example_minimal example_full_flow)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
  endforeach()
endif()

foreach(t example_minimal example_full_flow)
  target_compile_definitions(${t} PRIVATE
    ALERT_ENGINE_POLICY_DIR="${CMAKE_SOURCE_DIR}/policies/mapapp")
endforeach()
