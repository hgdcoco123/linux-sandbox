cmake_minimum_required(VERSION 3.20)
project(linux_sandbox VERSION 0.5.3 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
add_library(sandbox_dependencies INTERFACE)
target_include_directories(sandbox_dependencies INTERFACE third_party third_party/spdlog/include)
# Redesigned core: typed business model; no JSON in the business types.
add_library(sandbox_core src/core/ids.cpp src/core/outcome.cpp src/core/result.cpp
  src/core/request.cpp src/core/ops.cpp src/core/wire.cpp src/core/supervisor.cpp
  src/core/prepare.cpp src/core/control.cpp src/core/workspace.cpp src/core/ops_json.cpp
  src/core/record_json.cpp)
target_include_directories(sandbox_core PUBLIC include)
target_link_libraries(sandbox_core PUBLIC sandbox_dependencies)
add_executable(core_tests tests/unit/core_tests.cpp)
target_link_libraries(core_tests PRIVATE sandbox_core)
add_test(NAME core COMMAND core_tests)
# These suites call decoders on purpose to assert that they throw; the result is
# meant to be discarded, so the warning is noise here and hides real ones.
foreach(target core_tests wire_tests control_tests fault_tests)
  if(TARGET ${target} AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wno-unused-result)
  endif()
endforeach()
add_executable(wire_tests tests/experimental/wire_tests.cpp)
target_link_libraries(wire_tests PRIVATE sandbox_core)
add_test(NAME wire COMMAND wire_tests)
add_executable(supervisor_tests tests/unit/supervisor_tests.cpp)
target_link_libraries(supervisor_tests PRIVATE sandbox_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # GCC 15 reports a false positive inside libstdc++ variant/string copies that
  # this test instantiates; the library itself builds warning-free.
  target_compile_options(supervisor_tests PRIVATE -Wno-maybe-uninitialized)
endif()
add_test(NAME supervisor COMMAND supervisor_tests)
add_executable(prepare_tests tests/unit/prepare_tests.cpp)
target_link_libraries(prepare_tests PRIVATE sandbox_core)
add_test(NAME prepare COMMAND prepare_tests)
add_executable(control_tests tests/unit/control_tests.cpp)
target_link_libraries(control_tests PRIVATE sandbox_core)
add_test(NAME control COMMAND control_tests)
add_executable(fault_tests tests/unit/fault_tests.cpp)
target_link_libraries(fault_tests PRIVATE sandbox_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fault_tests PRIVATE -Wno-maybe-uninitialized)
endif()
add_test(NAME fault COMMAND fault_tests)
add_executable(workspace_tests tests/unit/workspace_tests.cpp)
target_link_libraries(workspace_tests PRIVATE sandbox_core)
add_test(NAME workspace COMMAND workspace_tests)
add_executable(ops_json_tests tests/unit/ops_json_tests.cpp)
target_link_libraries(ops_json_tests PRIVATE sandbox_core)
add_test(NAME ops_json COMMAND ops_json_tests)
add_library(sandbox_model src/model/config.cpp src/model/report.cpp src/model/command_model.cpp src/model/task.cpp)
target_include_directories(sandbox_model PUBLIC include)
target_link_libraries(sandbox_model PUBLIC sandbox_dependencies sandbox_core)
add_executable(model_tests tests/unit/model_tests.cpp)
target_link_libraries(model_tests PRIVATE sandbox_model)
add_test(NAME model COMMAND model_tests)
add_library(sandbox_metrics src/resources/metrics.cpp)
target_link_libraries(sandbox_metrics PUBLIC sandbox_model)
add_executable(metrics_tests tests/unit/metrics_tests.cpp)
target_link_libraries(metrics_tests PRIVATE sandbox_metrics)
add_test(NAME metrics COMMAND metrics_tests)
add_executable(session_model_tests tests/unit/session_model_tests.cpp)
target_link_libraries(session_model_tests PRIVATE sandbox_model)
add_test(NAME session_model COMMAND session_model_tests)
add_executable(session_contract_tests tests/unit/session_contract_tests.cpp)
target_link_libraries(session_contract_tests PRIVATE sandbox_model)
add_test(NAME session_contract COMMAND session_contract_tests)
find_package(Threads REQUIRED)
add_library(sandbox_writer src/runtime/bounded_writer.cpp)
target_include_directories(sandbox_writer PUBLIC include)
target_link_libraries(sandbox_writer PUBLIC Threads::Threads)
add_executable(writer_tests tests/unit/writer_tests.cpp)
target_link_libraries(writer_tests PRIVATE sandbox_writer)
add_test(NAME writer COMMAND writer_tests)
set_tests_properties(writer PROPERTIES TIMEOUT 15)
add_executable(task_tests tests/unit/task_tests.cpp)
target_link_libraries(task_tests PRIVATE sandbox_model)
add_test(NAME task_model COMMAND task_tests)
add_executable(writer_benchmark benchmarks/writer_benchmark.cpp)
target_link_libraries(writer_benchmark PRIVATE sandbox_writer sandbox_model)
# Labels classify existing tests without changing their registration or source paths.
if(BUILD_TESTING)
  set_tests_properties(core supervisor prepare control fault workspace ops_json model metrics
    session_model session_contract writer task_model PROPERTIES LABELS "unit;portable")
  set_tests_properties(wire PROPERTIES LABELS "experimental;portable")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "Linux runtime is disabled on this host; portable model tests remain available")
  return()
endif()
find_package(Threads REQUIRED)
# A release tarball should run on any glibc system, not only where libprotobuf and libseccomp
# happen to be installed at matching sonames. Turning this on links both statically; what stays
# dynamic is only libc/libstdc++/libgcc/libm, which every Linux has.
option(SANDBOX_STATIC_DEPS "Link libseccomp and protobuf statically (portable binary)" OFF)
if(SANDBOX_STATIC_DEPS)
  set(Protobuf_USE_STATIC_LIBS ON)
endif()
find_path(SECCOMP_INCLUDE_DIR seccomp.h REQUIRED)
if(SANDBOX_STATIC_DEPS)
  find_library(SECCOMP_LIBRARY NAMES libseccomp.a seccomp REQUIRED)
else()
  find_library(SECCOMP_LIBRARY seccomp REQUIRED)
endif()
find_package(Protobuf 3.21 REQUIRED)
protobuf_generate_cpp(SESSION_PROTO_SOURCES SESSION_PROTO_HEADERS proto/session_v1.proto)
add_library(sandbox_runtime
  src/platform/linux.cpp src/platform/environment.cpp src/runtime/protocol.cpp src/storage/run_store.cpp
  src/resources/cgroup.cpp src/runtime/output_capture.cpp
  src/runtime/launcher_client.cpp src/runtime/run_session.cpp
  src/isolation/rootfs.cpp src/isolation/security.cpp src/runtime/init.cpp src/runtime/init_common.cpp
  src/application/service.cpp
  src/runtime/interactive_session.cpp src/storage/artifacts.cpp
  src/runtime/session_protocol.cpp src/runtime/session_wire.cpp ${SESSION_PROTO_SOURCES} src/storage/session_store.cpp src/storage/workspace_service.cpp
  src/platform/event_loop.cpp src/runtime/batch.cpp)
target_include_directories(sandbox_runtime PUBLIC include)
target_include_directories(sandbox_runtime PRIVATE ${SECCOMP_INCLUDE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(sandbox_runtime PUBLIC _GNU_SOURCE)
target_compile_options(sandbox_runtime PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sandbox_runtime PUBLIC sandbox_model sandbox_metrics sandbox_writer sandbox_core
  Threads::Threads ${SECCOMP_LIBRARY} protobuf::libprotobuf)
add_executable(sandbox apps/cli/main.cpp)
# The version comes from project() so the CLI and any package metadata cannot drift apart.
target_compile_definitions(sandbox PRIVATE SBX_VERSION="${PROJECT_VERSION}")
target_link_libraries(sandbox PRIVATE sandbox_runtime)
add_executable(sandbox-launcher apps/launcher/main.cpp)
target_link_libraries(sandbox-launcher PRIVATE sandbox_runtime)
add_executable(sandbox-fixture tests/fixtures/probe.cpp)
target_link_options(sandbox-fixture PRIVATE -static)
install(TARGETS sandbox sandbox-launcher RUNTIME DESTINATION bin)
if(BUILD_TESTING)
  add_executable(control_fd_tests tests/integration/control_fd_tests.cpp)
  target_link_libraries(control_fd_tests PRIVATE sandbox_runtime)
  add_test(NAME control_fd COMMAND control_fd_tests)
  add_executable(session_wire_tests tests/integration/session_wire_tests.cpp)
  target_link_libraries(session_wire_tests PRIVATE sandbox_runtime)
  add_test(NAME session_wire COMMAND session_wire_tests)
  add_executable(output_runtime_tests tests/integration/output_runtime_tests.cpp)
  target_link_libraries(output_runtime_tests PRIVATE sandbox_runtime)
  add_test(NAME output_runtime COMMAND output_runtime_tests)
  set_tests_properties(output_runtime PROPERTIES TIMEOUT 15)
  add_executable(session_store_tests tests/integration/session_store_tests.cpp)
  target_link_libraries(session_store_tests PRIVATE sandbox_runtime)
  add_test(NAME session_store COMMAND session_store_tests)
  add_executable(publish_tests tests/integration/publish_tests.cpp)
  target_link_libraries(publish_tests PRIVATE sandbox_runtime)
  add_test(NAME publish COMMAND publish_tests)
  set_tests_properties(publish PROPERTIES TIMEOUT 30)
  add_executable(mount_source_tests tests/integration/mount_source_tests.cpp)
  target_link_libraries(mount_source_tests PRIVATE sandbox_runtime)
  add_test(NAME mount_source COMMAND mount_source_tests)
  add_executable(session_runtime_tests tests/integration/session_runtime_tests.cpp)
  target_link_libraries(session_runtime_tests PRIVATE sandbox_runtime)
  add_test(NAME session_runtime COMMAND session_runtime_tests)
  set_tests_properties(session_runtime PROPERTIES TIMEOUT 10)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME deployment COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/deployment_tests.py)
  add_test(NAME environment_diagnostics COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/environment_diagnostics.py $<TARGET_FILE:sandbox>)
  set_tests_properties(environment_diagnostics PROPERTIES TIMEOUT 10)
  add_test(NAME session_sdk COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/session_sdk_tests.py)
  add_test(NAME session_wire_python COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/session_wire_tests.py)
  add_executable(session_wire_bridge tests/fixtures/session_wire_bridge.cpp)
  target_link_libraries(session_wire_bridge PRIVATE sandbox_runtime)
  add_test(NAME session_wire_interop COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/session_wire_interop_tests.py $<TARGET_FILE:session_wire_bridge>)
  add_test(NAME launch_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/launch_contract_tests.py)
  add_test(NAME batch_sdk COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/batch_sdk_tests.py)
  add_test(NAME session_prepare COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/session_prepare.py $<TARGET_FILE:sandbox>)
  set_tests_properties(session_prepare PROPERTIES TIMEOUT 60)
  add_test(NAME sessions COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/sessions.py $<TARGET_FILE:sandbox>)
  set_tests_properties(sessions PROPERTIES TIMEOUT 120)
  add_test(NAME batch_integration COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/batch_integration.py $<TARGET_FILE:sandbox>)
  set_tests_properties(batch_integration PROPERTIES TIMEOUT 120)
  # The analysis layer is Python, but its contracts belong to the same suite: evidence
  # bounds, citation validation, tool budgets, retrieval and the verification launch plan.
  add_test(NAME ai_contracts COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests/unit/ai)
  set_tests_properties(ai_contracts PROPERTIES TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME process_integration COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/integration.py $<TARGET_FILE:sandbox>)
  set_tests_properties(process_integration PROPERTIES TIMEOUT 120)
  add_test(NAME isolation_integration COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/acceptance/isolation.py $<TARGET_FILE:sandbox>)
  set_tests_properties(isolation_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

  set_tests_properties(control_fd session_wire output_runtime session_store publish mount_source
    session_runtime session_wire_python session_wire_interop environment_diagnostics
    session_prepare sessions batch_integration process_integration
    PROPERTIES LABELS "integration;linux")
  set_tests_properties(session_sdk batch_sdk PROPERTIES LABELS "unit;linux")
  set_tests_properties(deployment launch_contract PROPERTIES LABELS "deployment;linux")
  set_tests_properties(isolation_integration PROPERTIES LABELS "acceptance;linux")
  set_property(TEST process_integration sessions supervisor isolation_integration publish
    APPEND PROPERTY LABELS onboarding)

  # These tests ignore [[nodiscard]] results on purpose - the point of the case is
  # that the call throws. The flag has to be attached to targets that already exist,
  # which is why it sits here and not in the list near the top of the file: adding it
  # there silently did nothing.
  foreach(target wire_tests control_tests fault_tests session_wire_tests session_runtime_tests)
    target_compile_options(${target} PRIVATE -Wno-unused-result)
  endforeach()
endif()
