
#include "unwind.h"
#include "__cxxabi_config.h"
extern "C" _Unwind_Reason_Code __gxx_personality_v0_(int version, _Unwind_Action actions, uint64_t exceptionClass, _Unwind_Exception* unwind_exception, _Unwind_Context* context);
extern "C" _Unwind_Reason_Code _LIBCXXABI_FUNC_VIS __gxx_personality_v0(int version, _Unwind_Action actions, uint64_t exceptionClass, _Unwind_Exception* unwind_exception, _Unwind_Context* context) {
  return __gxx_personality_v0_(version, actions, exceptionClass, unwind_exception, context);
}

