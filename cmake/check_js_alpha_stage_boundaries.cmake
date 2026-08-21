if(NOT SOURCE_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

set(S05_FILES
  "${SOURCE_ROOT}/include/quickapp/js/binding/alpha_initial_binding_stage.h"
  "${SOURCE_ROOT}/src/binding/alpha_initial_binding_stage.cpp"
)
foreach(FILE_PATH IN LISTS S05_FILES)
  file(READ "${FILE_PATH}" CONTENT)
  foreach(FORBIDDEN IN ITEMS
      "InstantiateTemplate" "instantiateTemplate" "JsRequestIdAllocator"
      "SubmitRenderTransaction" "RenderTransaction" "Navigation"
      "Capability" "PageIR" "quickjs.h" "std::ifstream" "fopen(")
    string(FIND "${CONTENT}" "${FORBIDDEN}" FOUND)
    if(NOT FOUND EQUAL -1)
      message(FATAL_ERROR "Alpha JS-S05 boundary violation in ${FILE_PATH}: ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

set(S07_FILES
  "${SOURCE_ROOT}/include/quickapp/js/render/alpha_initial_transaction_builder.h"
  "${SOURCE_ROOT}/src/render/alpha_initial_transaction_builder.cpp"
)
foreach(FILE_PATH IN LISTS S07_FILES)
  file(READ "${FILE_PATH}" CONTENT)
  foreach(FORBIDDEN IN ITEMS
      "ModuleLoader" "bindingEvaluatorsOnExecutor" "evaluator" "Reactive"
      "SubmitRenderTransaction" "RenderTransaction" "Navigation"
      "Capability" "PageIR" "quickjs.h" "std::ifstream" "fopen(")
    string(FIND "${CONTENT}" "${FORBIDDEN}" FOUND)
    if(NOT FOUND EQUAL -1)
      message(FATAL_ERROR "Alpha JS-S07 boundary violation in ${FILE_PATH}: ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

message(STATUS "Alpha JS-S05/JS-S07 ownership boundaries passed")
