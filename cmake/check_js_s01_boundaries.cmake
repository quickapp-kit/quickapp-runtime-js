file(GLOB_RECURSE PUBLIC_AND_COMMON_FILES
  "${SOURCE_ROOT}/include/*"
  "${SOURCE_ROOT}/src/*"
)

foreach(FILE_PATH IN LISTS PUBLIC_AND_COMMON_FILES)
  if(IS_DIRECTORY "${FILE_PATH}")
    continue()
  endif()
  if(FILE_PATH MATCHES "/libuv_event_loop_backend\\.cpp$")
    continue()
  endif()
  file(READ "${FILE_PATH}" CONTENT)
  if(CONTENT MATCHES "quickjs\\.h|JSRuntime|JSContext|JSValue|JNI|UIKit|LVGL|libuv")
    message(FATAL_ERROR "JS-S01 boundary leak in ${FILE_PATH}")
  endif()
endforeach()

if(NOT EXISTS "${PROBE}")
  message(FATAL_ERROR "composition probe is missing: ${PROBE}")
endif()

execute_process(
  COMMAND "${NM}" "${PROBE}"
  RESULT_VARIABLE NM_RESULT
  OUTPUT_VARIABLE SYMBOLS
  ERROR_VARIABLE NM_ERROR
)
if(NOT NM_RESULT EQUAL 0)
  message(FATAL_ERROR "nm failed: ${NM_ERROR}")
endif()
if(SYMBOLS MATCHES "FakeEngine|engine\\.fake")
  message(FATAL_ERROR "production composition contains Fake Engine symbols")
endif()

file(READ "${MANIFEST}" MANIFEST_CONTENT)
string(REGEX MATCHALL "\"category\"[ \t]*:[ \t]*\"engine\"" ENGINE_MODULES "${MANIFEST_CONTENT}")
list(LENGTH ENGINE_MODULES ENGINE_MODULE_COUNT)
if(NOT ENGINE_MODULE_COUNT EQUAL 1)
  message(FATAL_ERROR "composition must contain exactly one engine module")
endif()
string(REGEX MATCHALL "\"moduleId\"[ \t]*:[ \t]*\"runtime\\.js-framework\"" JS_FRAMEWORK_MODULES "${MANIFEST_CONTENT}")
list(LENGTH JS_FRAMEWORK_MODULES JS_FRAMEWORK_MODULE_COUNT)
if(NOT JS_FRAMEWORK_MODULE_COUNT EQUAL 1)
  message(FATAL_ERROR "composition must contain exactly one runtime.js-framework module")
endif()
