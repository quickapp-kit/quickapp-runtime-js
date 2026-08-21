set(S03_FILES
  "${SOURCE_ROOT}/include/quickapp/js/module/module_loader.h"
  "${SOURCE_ROOT}/src/module/module_loader.cpp"
)

foreach(FILE_PATH IN LISTS S03_FILES)
  file(READ "${FILE_PATH}" CONTENT)
  foreach(FORBIDDEN IN ITEMS
      "quickjs.h" "JSValue" "JNIEnv" "UIKit" "LVGL" "lvgl.h"
      "PackageSource" "PageIR" "std::ifstream" "fopen(" "open(")
    string(FIND "${CONTENT}" "${FORBIDDEN}" FOUND)
    if(NOT FOUND EQUAL -1)
      message(FATAL_ERROR "JS-S03 boundary violation in ${FILE_PATH}: ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

file(READ "${SOURCE_ROOT}/include/quickapp/js/abi/runtime_abi_service.h" ABI_HEADER)
string(FIND "${ABI_HEADER}" "$app_define$" MODULE_ABI_IN_RUNTIME_ABI)
if(NOT MODULE_ABI_IN_RUNTIME_ABI EQUAL -1)
  message(FATAL_ERROR "Module ABI leaked into Runtime ABI service")
endif()

message(STATUS "JS-S03 boundary scan passed")
