file(GLOB_RECURSE ABI_PUBLIC "${SOURCE_ROOT}/include/quickapp/js/abi/*")
file(GLOB_RECURSE ABI_SOURCE "${SOURCE_ROOT}/src/abi/*")
set(ABI_FILES ${ABI_PUBLIC} ${ABI_SOURCE})

foreach(FILE IN LISTS ABI_FILES)
  file(READ "${FILE}" CONTENT)
  foreach(FORBIDDEN
          "quickjs.h"
          "JsRequestIdAllocator"
          "completionToken"
          "bytesBase64"
          "JSON.parse"
          "JSON.stringify"
          "jni.h"
          "UIKit"
          "LVGL")
    string(FIND "${CONTENT}" "${FORBIDDEN}" FOUND)
    if(NOT FOUND EQUAL -1)
      message(FATAL_ERROR "JS-S02 boundary violation ${FORBIDDEN} in ${FILE}")
    endif()
  endforeach()
endforeach()

file(READ "${SOURCE_ROOT}/include/quickapp/js/abi/runtime_abi_types.h" TYPES)
foreach(FORBIDDEN
        "CoreMessage<"
        "JsCallbackMessage<"
        "RuntimeValue::Object fields"
        "RuntimeValue payload"
        "std::string module, method"
        "RuntimeValue::Array args")
  string(FIND "${TYPES}" "${FORBIDDEN}" FOUND)
  if(NOT FOUND EQUAL -1)
    message(FATAL_ERROR "Generic message model restored: ${FORBIDDEN}")
  endif()
endforeach()

foreach(REQUIRED
        "struct InstantiateTemplate"
        "struct CompleteLifecycle"
        "struct LoadVerifiedModule"
        "struct SurfaceStatusChanged"
        "using CoreInboundMessage ="
        "using JsInboundMessage =")
  string(FIND "${TYPES}" "${REQUIRED}" FOUND)
  if(FOUND EQUAL -1)
    message(FATAL_ERROR "Concrete closed message model missing: ${REQUIRED}")
  endif()
endforeach()

foreach(REQUIRED
        "using ImmutableByteStorage ="
        "std::shared_ptr<const std::vector<std::uint8_t>>"
        "ImmutableByteStorage bytes;")
  string(FIND "${TYPES}" "${REQUIRED}" FOUND)
  if(FOUND EQUAL -1)
    message(FATAL_ERROR "Immutable ModuleBundle storage missing: ${REQUIRED}")
  endif()
endforeach()

foreach(REQUIRED "key;" "expectedResultKind" "owner;" "ownerGeneration")
  string(FIND "${TYPES}" "${REQUIRED}" FOUND)
  if(FOUND EQUAL -1)
    message(FATAL_ERROR "PendingRecord field missing: ${REQUIRED}")
  endif()
endforeach()

message(STATUS "JS-S02 boundary scan passed")
