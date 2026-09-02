cmake_minimum_required(VERSION 3.24)
project(quickapp_runtime_js_framework LANGUAGES NONE)

option(QUICKAPP_JS_BUILD_TESTS "Run pure JS Framework source checks" ON)

if(QUICKAPP_JS_BUILD_TESTS)
  find_program(QUICKAPP_JS_NODE_EXECUTABLE node)
  if(NOT QUICKAPP_JS_NODE_EXECUTABLE)
    message(FATAL_ERROR "Node.js is required to validate the JS Framework source boundary")
  endif()

  add_custom_target(quickapp_js_framework_inventory ALL
    COMMAND "${QUICKAPP_JS_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/source-boundary.test.mjs"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)

  enable_testing()
  add_test(NAME js_framework_source_boundary
    COMMAND "${QUICKAPP_JS_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/source-boundary.test.mjs")
  set_tests_properties(js_framework_source_boundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
