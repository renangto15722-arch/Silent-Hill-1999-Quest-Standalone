/* SH_QUEST: small POSIX shims for Windows names the port calls. */
#if defined(__ANDROID__)
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>

int _mkdir(const char* dirname)
{
    return mkdir(dirname, 0777);
}

/* TEMP DIAG: log the call stack (offsets inside libmain.so, symbolize with
 * llvm-addr2line on the unstripped .so) under a tag. */
#include <unwind.h>
#include <dlfcn.h>
#include <stdint.h>
extern void VrOvr_Log(const char* fmt, ...);
typedef struct { uintptr_t pcs[24]; int n; } PcBtState;
static _Unwind_Reason_Code pc_bt_cb(struct _Unwind_Context* ctx, void* arg)
{
    PcBtState* st = (PcBtState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && st->n < 24) st->pcs[st->n++] = pc;
    return st->n < 24 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
void Pc_DiagBacktrace(const char* tag)
{
    PcBtState st;
    int i;
    st.n = 0;
    _Unwind_Backtrace(pc_bt_cb, &st);
    for (i = 0; i < st.n; i++)
    {
        Dl_info di;
        if (dladdr((void*)st.pcs[i], &di) && di.dli_fbase)
            VrOvr_Log("[VR-BT] %s #%d %s+0x%lx (%s)\n", tag, i,
                      di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?",
                      (unsigned long)(st.pcs[i] - (uintptr_t)di.dli_fbase),
                      di.dli_sname ? di.dli_sname : "");
        else
            VrOvr_Log("[VR-BT] %s #%d 0x%lx\n", tag, i, (unsigned long)st.pcs[i]);
    }
}
#endif
