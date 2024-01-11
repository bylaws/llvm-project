
#include "unwind.h"
#include "__cxxabi_config.h"
extern "C" _Unwind_Reason_Code __gxx_personality_v0_(int version, _Unwind_Action actions, uint64_t exceptionClass, _Unwind_Exception* unwind_exception, _Unwind_Context* context);
extern "C" _Unwind_Reason_Code _LIBCXXABI_FUNC_VIS __gxx_personality_v0(int version, _Unwind_Action actions, uint64_t exceptionClass, _Unwind_Exception* unwind_exception, _Unwind_Context* context) {
  return __gxx_personality_v0_(version, actions, exceptionClass, unwind_exception, context);
}

extern "C" _LIBCXXABI_FUNC_VIS EXCEPTION_DISPOSITION
___gxx_personality_seh0(PEXCEPTION_RECORD ms_exc, void *this_frame,
                       PCONTEXT ms_orig_context, PDISPATCHER_CONTEXT ms_disp);
extern "C" _LIBCXXABI_FUNC_VIS EXCEPTION_DISPOSITION
__gxx_personality_seh0(PEXCEPTION_RECORD ms_exc, void *this_frame,
                       PCONTEXT ms_orig_context, PDISPATCHER_CONTEXT ms_disp)
{
  return ___gxx_personality_seh0(ms_exc, this_frame, ms_orig_context, ms_disp);
}
