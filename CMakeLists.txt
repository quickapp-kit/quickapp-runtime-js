cmake_minimum_required(VERSION 3.24)
project(quickapp_runtime_js LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(QUICKAPP_JS_BUILD_TESTS "Build JS Runtime contract tests" ON)
option(QUICKAPP_JS_BUILD_QUICKJS_PROVIDER "Build the QuickJS V1 provider" ON)
option(QUICKAPP_JS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(QUICKAPP_JS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(QUICKAPP_JS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

set(QUICKAPP_JS_QUICKJS_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../source/third_party/quickjs"
    CACHE PATH "Path to the vendored QuickJS source")

if(QUICKAPP_JS_BUILD_QUICKJS_PROVIDER)
  if(NOT EXISTS "${QUICKAPP_JS_QUICKJS_SOURCE_DIR}/quickjs.h")
    message(FATAL_ERROR "QuickJS source not found: ${QUICKAPP_JS_QUICKJS_SOURCE_DIR}")
  endif()
  file(STRINGS "${QUICKAPP_JS_QUICKJS_SOURCE_DIR}/VERSION" QUICKAPP_JS_QUICKJS_VERSION
       LIMIT_COUNT 1)
  configure_file(cmake/quickjs_config.h.in
                 generated/quickapp_quickjs_config.h @ONLY)
endif()

function(quickapp_js_apply_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

function(quickapp_js_apply_sanitizers target)
  if(QUICKAPP_JS_ENABLE_ASAN)
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
  if(QUICKAPP_JS_ENABLE_UBSAN)
    target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=undefined)
  endif()
  if(QUICKAPP_JS_ENABLE_TSAN)
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
  endif()
endfunction()

add_library(quickapp_js_engine_api STATIC
  src/engine/engine_types.cpp
  src/engine/runtime_value.cpp
)
target_include_directories(quickapp_js_engine_api PUBLIC include)
target_include_directories(quickapp_js_engine_api PRIVATE src)
quickapp_js_apply_warnings(quickapp_js_engine_api)
quickapp_js_apply_sanitizers(quickapp_js_engine_api)

add_library(quickapp_js_executor STATIC
  src/engine/js_executor.cpp
  src/engine/js_engine_service.cpp
  src/engine/observation.cpp
)
target_include_directories(quickapp_js_executor PUBLIC include)
target_include_directories(quickapp_js_executor PRIVATE src)
target_link_libraries(quickapp_js_executor PUBLIC quickapp_js_engine_api)
quickapp_js_apply_warnings(quickapp_js_executor)
quickapp_js_apply_sanitizers(quickapp_js_executor)

add_library(quickapp_js_runtime_abi STATIC
  src/abi/runtime_abi_types.cpp
  src/abi/runtime_abi_codec.cpp
  src/abi/runtime_abi_service.cpp
)
target_include_directories(quickapp_js_runtime_abi PUBLIC include)
target_link_libraries(quickapp_js_runtime_abi PUBLIC quickapp_js_executor)
quickapp_js_apply_warnings(quickapp_js_runtime_abi)
quickapp_js_apply_sanitizers(quickapp_js_runtime_abi)

add_library(quickapp_js_module_loader STATIC
  src/module/module_loader.cpp
)
target_include_directories(quickapp_js_module_loader PUBLIC include)
target_include_directories(quickapp_js_module_loader PRIVATE src)
target_link_libraries(quickapp_js_module_loader PUBLIC quickapp_js_runtime_abi)
quickapp_js_apply_warnings(quickapp_js_module_loader)
quickapp_js_apply_sanitizers(quickapp_js_module_loader)

add_library(quickapp_js_event STATIC
  src/event/handler_registry.cpp
)
target_include_directories(quickapp_js_event PUBLIC include)
target_link_libraries(quickapp_js_event PUBLIC quickapp_js_module_loader)
quickapp_js_apply_warnings(quickapp_js_event)
quickapp_js_apply_sanitizers(quickapp_js_event)

add_library(quickapp_js_static_facades STATIC
  src/framework/static_facade_catalog.cpp
)
target_include_directories(quickapp_js_static_facades PUBLIC include)
target_link_libraries(quickapp_js_static_facades PUBLIC
  quickapp_js_module_loader
)
quickapp_js_apply_warnings(quickapp_js_static_facades)
quickapp_js_apply_sanitizers(quickapp_js_static_facades)

add_library(quickapp_js_page_host_control STATIC
  src/page/page_host_control.cpp
)
target_include_directories(quickapp_js_page_host_control PUBLIC include)
target_link_libraries(quickapp_js_page_host_control PUBLIC
  quickapp_js_runtime_abi
)
quickapp_js_apply_warnings(quickapp_js_page_host_control)
quickapp_js_apply_sanitizers(quickapp_js_page_host_control)

add_library(quickapp_js_vm_lifecycle STATIC
  src/vm/vm_lifecycle_service.cpp
)
target_include_directories(quickapp_js_vm_lifecycle PUBLIC include)
target_include_directories(quickapp_js_vm_lifecycle PRIVATE src)
target_link_libraries(quickapp_js_vm_lifecycle PUBLIC
  quickapp_js_module_loader
  quickapp_js_runtime_abi
)
quickapp_js_apply_warnings(quickapp_js_vm_lifecycle)
quickapp_js_apply_sanitizers(quickapp_js_vm_lifecycle)

add_library(quickapp_js_alpha_initial_binding STATIC
  src/binding/alpha_initial_binding_stage.cpp
)
target_include_directories(quickapp_js_alpha_initial_binding PUBLIC include)
target_link_libraries(quickapp_js_alpha_initial_binding PUBLIC
  quickapp_js_module_loader
)
quickapp_js_apply_warnings(quickapp_js_alpha_initial_binding)
quickapp_js_apply_sanitizers(quickapp_js_alpha_initial_binding)

add_library(quickapp_js_alpha_initial_render STATIC
  src/render/alpha_initial_transaction_builder.cpp
)
target_include_directories(quickapp_js_alpha_initial_render PUBLIC include)
target_link_libraries(quickapp_js_alpha_initial_render PUBLIC
  quickapp_js_runtime_abi
)
quickapp_js_apply_warnings(quickapp_js_alpha_initial_render)
quickapp_js_apply_sanitizers(quickapp_js_alpha_initial_render)

add_library(quickapp_js_alpha_page_stage STATIC
  src/alpha/alpha_page_initialization_stage.cpp
)
target_include_directories(quickapp_js_alpha_page_stage PUBLIC include)
target_link_libraries(quickapp_js_alpha_page_stage PUBLIC
  quickapp_js_alpha_initial_binding
  quickapp_js_alpha_initial_render
)
quickapp_js_apply_warnings(quickapp_js_alpha_page_stage)
quickapp_js_apply_sanitizers(quickapp_js_alpha_page_stage)

if(QUICKAPP_JS_BUILD_QUICKJS_PROVIDER)
  add_library(quickapp_quickjs_vendor STATIC
  ${QUICKAPP_JS_QUICKJS_SOURCE_DIR}/quickjs.c
  ${QUICKAPP_JS_QUICKJS_SOURCE_DIR}/dtoa.c
  ${QUICKAPP_JS_QUICKJS_SOURCE_DIR}/libregexp.c
  ${QUICKAPP_JS_QUICKJS_SOURCE_DIR}/libunicode.c
  ${QUICKAPP_JS_QUICKJS_SOURCE_DIR}/cutils.c
  )
  target_include_directories(quickapp_quickjs_vendor PRIVATE ${QUICKAPP_JS_QUICKJS_SOURCE_DIR})
  target_compile_definitions(quickapp_quickjs_vendor PRIVATE
  _GNU_SOURCE
  CONFIG_VERSION="${QUICKAPP_JS_QUICKJS_VERSION}"
  )
  target_compile_options(quickapp_quickjs_vendor PRIVATE
  -fwrapv
  -Wno-sign-compare
  -Wno-unused-parameter
  -Wno-missing-field-initializers
  -Wno-implicit-fallthrough
  )
  if(UNIX)
    target_link_libraries(quickapp_quickjs_vendor PUBLIC m pthread)
  endif()
  quickapp_js_apply_sanitizers(quickapp_quickjs_vendor)

  add_library(quickapp_js_engine_quickjs STATIC
    providers/quickjs/src/quickjs_provider.cpp
  )
  target_include_directories(quickapp_js_engine_quickjs PUBLIC providers/quickjs/include)
  target_include_directories(quickapp_js_engine_quickjs PRIVATE
    include
    src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
  )
  target_link_libraries(quickapp_js_engine_quickjs
    PUBLIC quickapp_js_engine_api
    PRIVATE quickapp_quickjs_vendor
  )
  target_compile_definitions(quickapp_js_engine_quickjs PRIVATE
    QUICKAPP_QUICKJS_VERSION="${QUICKAPP_JS_QUICKJS_VERSION}"
  )
  quickapp_js_apply_warnings(quickapp_js_engine_quickjs)
  target_compile_options(quickapp_js_engine_quickjs PRIVATE
    -Wno-unused-parameter
    -Wno-c99-extensions
  )
  quickapp_js_apply_sanitizers(quickapp_js_engine_quickjs)
endif()

if(QUICKAPP_JS_BUILD_TESTS)
  if(NOT QUICKAPP_JS_BUILD_QUICKJS_PROVIDER)
    message(FATAL_ERROR "JS-S01 contract tests require the QuickJS provider")
  endif()
  enable_testing()

  add_library(quickapp_js_engine_fake STATIC fakes/src/fake_engine_provider.cpp)
  target_include_directories(quickapp_js_engine_fake PUBLIC fakes/include)
  target_include_directories(quickapp_js_engine_fake PRIVATE include src)
  target_link_libraries(quickapp_js_engine_fake PUBLIC quickapp_js_engine_api)
  quickapp_js_apply_warnings(quickapp_js_engine_fake)
  quickapp_js_apply_sanitizers(quickapp_js_engine_fake)

  add_executable(js_s01_contract_tests tests/js_s01_contract_tests.cpp)
  target_link_libraries(js_s01_contract_tests PRIVATE
    quickapp_js_executor
    quickapp_js_engine_fake
    quickapp_js_engine_quickjs
  )
  quickapp_js_apply_warnings(js_s01_contract_tests)
  quickapp_js_apply_sanitizers(js_s01_contract_tests)
  add_test(NAME js_s01_contract_tests COMMAND js_s01_contract_tests)

  add_executable(js_s02_contract_tests tests/js_s02_contract_tests.cpp)
  target_link_libraries(js_s02_contract_tests PRIVATE
    quickapp_js_runtime_abi
    quickapp_js_engine_fake
    quickapp_js_engine_quickjs
  )
  quickapp_js_apply_warnings(js_s02_contract_tests)
  quickapp_js_apply_sanitizers(js_s02_contract_tests)
  add_test(NAME js_s02_contract_tests COMMAND js_s02_contract_tests)

  add_executable(js_s03_module_loader_tests
    tests/js_s03_module_loader_tests.cpp
  )
  target_link_libraries(js_s03_module_loader_tests PRIVATE
    quickapp_js_module_loader
    quickapp_js_engine_fake
    quickapp_js_engine_quickjs
  )
  quickapp_js_apply_warnings(js_s03_module_loader_tests)
  quickapp_js_apply_sanitizers(js_s03_module_loader_tests)
  add_test(NAME js_s03_module_loader_tests COMMAND js_s03_module_loader_tests)

  add_executable(js_s04_vm_lifecycle_tests
    tests/js_s04_vm_lifecycle_tests.cpp
  )
  target_link_libraries(js_s04_vm_lifecycle_tests PRIVATE
    quickapp_js_vm_lifecycle
    quickapp_js_alpha_page_stage
    quickapp_js_static_facades
    quickapp_js_page_host_control
    quickapp_js_engine_fake
    quickapp_js_engine_quickjs
  )
  quickapp_js_apply_warnings(js_s04_vm_lifecycle_tests)
  quickapp_js_apply_sanitizers(js_s04_vm_lifecycle_tests)
  add_test(NAME js_s04_vm_lifecycle_tests COMMAND js_s04_vm_lifecycle_tests)

  add_test(
    NAME js_alpha_typed_facade_boundary_scan
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_js_alpha_typed_facade_boundaries.cmake
  )

  add_test(
    NAME js_s04_boundary_scan
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_js_s04_boundaries.cmake
  )

  add_test(
    NAME js_alpha_stage_boundary_scan
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_js_alpha_stage_boundaries.cmake
  )

  add_executable(js_s01_quickjs_composition_probe
    tools/quickjs_composition_probe.cpp
  )
  target_link_libraries(js_s01_quickjs_composition_probe PRIVATE
    quickapp_js_executor
    quickapp_js_engine_quickjs
  )
  quickapp_js_apply_warnings(js_s01_quickjs_composition_probe)
  quickapp_js_apply_sanitizers(js_s01_quickjs_composition_probe)
  add_test(NAME js_s01_quickjs_composition_probe
           COMMAND js_s01_quickjs_composition_probe)

  add_test(
    NAME js_s01_boundary_scan
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -DBINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}
      -DNM=${CMAKE_NM}
      -DPROBE=$<TARGET_FILE:js_s01_quickjs_composition_probe>
      -DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/evidence/runtime-composition.json
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_js_s01_boundaries.cmake
  )

  add_test(
    NAME js_s02_boundary_scan
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_js_s02_boundaries.cmake
  )

  add_test(
    NAME js_s03_boundary_scan
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_js_s03_boundaries.cmake
  )
endif()
