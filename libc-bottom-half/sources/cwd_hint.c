// Optional cwd-hint adapter for the `cwd_get_suggested` Preview 1 hostcall
// proposed at https://github.com/WebAssembly/WASI/issues/771.
//
// When this translation unit is linked into a guest, a startup constructor
// consults the host for a suggested current working directory via
// `cwd_get_suggested` and chdir()s to the returned path. If the host
// returns `errno::nosys` (the documented backward-compat path: hostcall is
// available but no hint was configured), the constructor leaves the cwd at
// its upstream default of "/".
//
// `getcwd.c` references this object's `__wasilibc_apply_cwd_hint` symbol
// as a *weak undefined* function pointer: when this file is linked in, the
// reference resolves and `getcwd(3)` reflects the hint; when it isn't
// linked in, the reference is NULL and `getcwd(3)` behaves identically to
// upstream — and the wasm module declares no `cwd_get_suggested` import,
// so it instantiates unchanged against runtimes that don't know about the
// hostcall.
//
// Crucially, the constructor goes through `chdir(3)` rather than assigning
// `__wasilibc_cwd` directly. Direct assignment "works" for `getcwd(3)` but
// not for any preopen-resolved path: `__wasilibc_find_relpath_alloc` (the
// thing that turns "foo" into a (preopen_fd, relative_path) pair at
// `openat`/`fstatat`/`readdir` time) lives in `chdir.c`. Without a strong
// reference to `chdir()`, the linker drops `chdir.c` entirely, the weak
// ref to `__wasilibc_find_relpath_alloc` stays NULL, and the resolver
// falls back to `__wasilibc_find_abspath`, which never consults
// `__wasilibc_cwd`. Guests that never call `chdir()` themselves (uutils,
// Rust std for `wasm32-wasip1`, ...) would therefore silently ignore the
// hint. Going through `chdir()` here is the single force-link that wires
// the whole cwd-aware path-resolution story together.
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

// Forward decl of chdir(3). Calling chdir() here (rather than assigning
// __wasilibc_cwd directly) is *load-bearing*: it creates a strong reference
// that pulls chdir.c into the link, which in turn provides
// __wasilibc_find_relpath_alloc. Without chdir.c linked, the weak ref to
// __wasilibc_find_relpath_alloc in libpreopen falls back to
// __wasilibc_find_abspath, which never consults __wasilibc_cwd — so any
// guest that doesn't otherwise call chdir() (e.g. uutils, Rust std for
// wasm32-wasip1) would silently ignore the hint. The chdir() call here is
// what makes the cwd hint actually reach openat()/fstatat()/readdir() at
// the preopen-resolution layer.
extern int chdir(const char *);

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

// Scratch buffer for the hint string handed to `chdir()`. Static so the
// constructor is safe to run before the heap is guaranteed to be ready;
// `chdir()` itself copies into `__wasilibc_cwd` so the buffer doesn't need
// to outlive the call.
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

    // Going through chdir() (not direct assignment) is required so chdir.c
    // is linked — see the comment on the extern decl above.
    (void)chdir(cwd_hint_buf);
}

// Eagerly seed the hint before main(). Constructor priority chosen to
// match `__wasilibc_initialize_environ_eagerly`'s slot (50) — the cwd
// hint is conceptually the same kind of host-provided process state as
// the environment block, and the same scheduling story applies.
__attribute__((constructor(50)))
static void __wasilibc_apply_cwd_hint_eagerly(void) {
    __wasilibc_apply_cwd_hint();
}
