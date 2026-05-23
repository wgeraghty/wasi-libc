// Optional cwd-hint adapter for the `cwd_get_suggested` Preview 1 hostcall
// proposed at https://github.com/WebAssembly/WASI/issues/771.
//
// When this translation unit is linked into a guest, a startup constructor
// consults the host for a suggested current working directory via
// `cwd_get_suggested` and uses the returned path as the initial value of
// `__wasilibc_cwd`. If the host returns `errno::nosys` (the documented
// backward-compat path: hostcall is available but no hint was configured),
// the constructor leaves `__wasilibc_cwd` at its upstream default of "/".
//
// `getcwd.c` references this object's `__wasilibc_apply_cwd_hint` symbol
// as a *weak undefined* function pointer: when this file is linked in, the
// reference resolves and `getcwd(3)` reflects the hint; when it isn't
// linked in, the reference is NULL and `getcwd(3)` behaves identically to
// upstream — and the wasm module declares no `cwd_get_suggested` import,
// so it instantiates unchanged against runtimes that don't know about the
// hostcall.
//
// The opt-in lever is therefore "did you link cwd_hint.o into your guest?"
// The patched sysroot baked alongside this branch always includes it; an
// unpatched sysroot never does. Wasm has no notion of weak imports — a
// declared import that the host can't resolve hard-fails instantiation —
// so this object-file-level opt-in is the only honest backward-compat
// story available at the libc layer.

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wasi/api.h>

// Forward decls of the cwd state owned by getcwd.c.
extern char *__wasilibc_cwd;
#ifdef _REENTRANT
void __wasilibc_cwd_lock(void);
void __wasilibc_cwd_unlock(void);
#else
#define __wasilibc_cwd_lock() (void)0
#define __wasilibc_cwd_unlock() (void)0
#endif

// Direct wasm import of the new hostcall. Kept private to this object
// file — the public `<wasi/api.h>` header is unchanged. Mirrors the
// private-import shape used by descriptor_table.c.
__attribute__((__import_module__("wasi_snapshot_preview1"),
               __import_name__("cwd_get_suggested")))
extern int32_t __imported_wasi_snapshot_preview1_cwd_get_suggested(
    int32_t buf, int32_t buf_len, int32_t retptr0);

// Largest hint we'll request. POSIX PATH_MAX-ish; the host truncates
// (path_readlink semantics) and reports the actual write count, so this
// is just a bound on the static scratch buffer.
#define CWD_HINT_MAX 4096

// Storage for the hint. Made into a strong allocation (a static array,
// not a malloc) so the constructor is safe to run before the heap is
// guaranteed to be initialized. `__wasilibc_cwd` is a `char *`, so we
// point it at this buffer after writing.
static char cwd_hint_buf[CWD_HINT_MAX + 1];

// Invoked by getcwd.c via a weak undefined ref. Safe to call any number
// of times; subsequent calls are cheap no-ops once the hint has been
// applied (or rejected).
void __wasilibc_apply_cwd_hint(void) {
    static int applied = 0;
    if (applied) {
        return;
    }
    applied = 1;

    __wasi_size_t written = 0;
    int32_t rc = __imported_wasi_snapshot_preview1_cwd_get_suggested(
        (int32_t)(uintptr_t)cwd_hint_buf,
        (int32_t)CWD_HINT_MAX,
        (int32_t)(uintptr_t)&written);
    if (rc != 0) {
        // errno::nosys (documented) or any other host error: keep the
        // upstream "/" default. Nothing to do.
        return;
    }
    if (written == 0 || written > CWD_HINT_MAX) {
        // Defensive: an empty hint isn't a valid cwd, and a length larger
        // than our buffer would mean the host violated the protocol.
        // Either way, fall through to the "/" default.
        return;
    }

    cwd_hint_buf[written] = '\0';

    __wasilibc_cwd_lock();
    __wasilibc_cwd = cwd_hint_buf;
    __wasilibc_cwd_unlock();
}

// Eagerly seed the hint before main(). Constructor priority chosen to
// match `__wasilibc_initialize_environ_eagerly`'s slot (50) — the cwd
// hint is conceptually the same kind of host-provided process state as
// the environment block, and the same scheduling story applies.
__attribute__((constructor(50)))
static void __wasilibc_apply_cwd_hint_eagerly(void) {
    __wasilibc_apply_cwd_hint();
}
