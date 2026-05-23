#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "lock.h"

char *__wasilibc_cwd = "/";

#ifdef _REENTRANT
static volatile int lock[1];
void __wasilibc_cwd_lock(void) { LOCK(lock); }
void __wasilibc_cwd_unlock(void) { UNLOCK(lock); }
#else
#define __wasilibc_cwd_lock() (void)0
#define __wasilibc_cwd_unlock() (void)0
#endif

// In this fork the cwd-hint adapter is always linked alongside `getcwd.c`
// (cwd_hint.c is part of `libc.a` for p1), so this is a strong external
// reference that the linker resolves at link time. Pulling in
// `cwd_hint.o` from this reference is intentional: it ensures every p1
// guest built against this sysroot consults the host `cwd_get_suggested`
// Preview 1 hostcall before `getcwd` returns. The trade-off — guests
// linked against this sysroot will fail to instantiate on runtimes that
// don't implement the hostcall — is the design's whole point; see
// cwd_hint.c for the full rationale.
//
// p2 doesn't get the hint: cwd_hint.c declares a wasi_snapshot_preview1
// import which is meaningless under the component-model interface. The
// equivalent shape would be a separate wit interface; that isn't in
// scope for this branch. The Makefile excludes cwd_hint.c from the p2
// build, so the call below is preprocessor-guarded to match.
//
// (Upstreaming this hostcall would want a different opt-in mechanism
// here: a weak reference from `getcwd.c` plus a separate
// `libwasi-cwd-hint.a` the consumer explicitly links via
// `-lwasi-cwd-hint`, mirroring the `libwasi-emulated-*` pattern. That
// isn't this fork's concern.)
#ifndef __wasilibc_use_wasip2
extern void __wasilibc_apply_cwd_hint(void);
#endif

char *getcwd(char *buf, size_t size)
{
#ifndef __wasilibc_use_wasip2
    __wasilibc_apply_cwd_hint();
#endif
    __wasilibc_cwd_lock();
    if (!buf) {
        buf = strdup(__wasilibc_cwd);
        if (!buf) {
            errno = ENOMEM;
            __wasilibc_cwd_unlock();
            return NULL;
        }
    } else {
        size_t len = strlen(__wasilibc_cwd);
        if (size < len + 1) {
            errno = ERANGE;
            __wasilibc_cwd_unlock();
            return NULL;
        }
        strcpy(buf, __wasilibc_cwd);
    }
    __wasilibc_cwd_unlock();
    return buf;
}

