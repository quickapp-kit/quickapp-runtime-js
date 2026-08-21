if(NOT SOURCE_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

set(FACADE_FILES
  "${SOURCE_ROOT}/include/quickapp/js/framework/static_facade_catalog.h"
  "${SOURCE_ROOT}/src/framework/static_facade_catalog.cpp"
  "${SOURCE_ROOT}/include/quickapp/js/page/page_host_control.h"
  "${SOURCE_ROOT}/src/page/page_host_control.cpp"
)

foreach(FILE_PATH IN LISTS FACADE_FILES)
  file(READ "${FILE_PATH}" CONTENT)
  foreach(FORBIDDEN IN ITEMS
      "moduleName" "methodName" "methodArgs" "JSON.stringify"
      "SubmitRenderTransaction" "RegisterHandler" "NavigationPush"
      "NavigationClose" "ShowToast" "DeviceGetInfo" "quickjs.h"
      "std::ifstream" "fopen(")
    string(FIND "${CONTENT}" "${FORBIDDEN}" FOUND)
    if(NOT FOUND EQUAL -1)
      message(FATAL_ERROR "Alpha typed facade boundary violation in ${FILE_PATH}: ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

message(STATUS "Alpha typed facade boundaries passed")
