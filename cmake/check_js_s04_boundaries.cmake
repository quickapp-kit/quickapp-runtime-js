if(NOT SOURCE_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()
set(S04_FILES
  "${SOURCE_ROOT}/include/quickapp/js/vm/vm_lifecycle_service.h"
  "${SOURCE_ROOT}/src/vm/vm_lifecycle_service.cpp"
)
foreach(FILE_PATH IN LISTS S04_FILES)
  file(READ "${FILE_PATH}" CONTENT)
  foreach(FORBIDDEN IN ITEMS
      "VNode" "RenderTransaction" "SubmitRenderTransaction" "Navigation"
      "Capability" "LVGL" "JNIEnv" "PageIR" "quickjs.h"
      "InstantiateTemplate" "bindingEvaluatorsOnExecutor"
      "JsRequestIdAllocator"
      "$page" "setTitleBar" "setMeta"
      "std::ifstream" "fopen(" "open(")
    string(FIND "${CONTENT}" "${FORBIDDEN}" FOUND)
    if(NOT FOUND EQUAL -1)
      message(FATAL_ERROR "JS-S04 Alpha boundary violation in ${FILE_PATH}: ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()
message(STATUS "JS-S04 Alpha boundary scan passed")
