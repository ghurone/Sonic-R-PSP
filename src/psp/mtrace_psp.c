/**
 * mtrace_psp.c — MTRACE=1 builds (psp/Makefile): every allocation of 32 KB or
 * more, and its free, in the log with who asked — to see what leaves the heap
 * in pieces on a PSP-1000. The calls from the game's and the port's objects
 * go through here (-Wl,--wrap); newlib's own don't.
 */
#ifdef SONICR_MTRACE
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t s);
void *__real_realloc(void *p, size_t n);
void *__real_memalign(size_t a, size_t n);
void __real_free(void *p);

#define MT_MIN (32 * 1024)

/* the big blocks live, to name a free (only those are logged) */
#define MT_SLOTS 256
static void *s_mtPtr[MT_SLOTS];
static unsigned int s_mtSize[MT_SLOTS];

static void mt_note(const char *what, void *p, size_t n, void *caller)
{
    if (p == NULL) {
        fprintf(stderr, "mtrace: %s %u KB FAILED from %p\n", what, (unsigned)(n / 1024), caller);
        return;
    }
    if (n < MT_MIN) {
        return;
    }
    for (int i = 0; i < MT_SLOTS; i++) {
        if (s_mtPtr[i] == NULL) {
            s_mtPtr[i] = p;
            s_mtSize[i] = (unsigned)n;
            break;
        }
    }
    struct mallinfo mi = mallinfo();
    fprintf(stderr, "mtrace: %s %u KB at %p from %p (in use %d KB, grown %d KB)\n", what, (unsigned)(n / 1024), p,
            caller, mi.uordblks / 1024, mi.arena / 1024);
}

static void mt_forget(void *p, void *caller)
{
    for (int i = 0; i < MT_SLOTS; i++) {
        if (s_mtPtr[i] == p && p != NULL) {
            fprintf(stderr, "mtrace: free %u KB at %p from %p\n", s_mtSize[i] / 1024, p, caller);
            s_mtPtr[i] = NULL;
            return;
        }
    }
}

void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    mt_note("malloc", p, n, __builtin_return_address(0));
    return p;
}

void *__wrap_calloc(size_t n, size_t s)
{
    void *p = __real_calloc(n, s);
    mt_note("calloc", p, n * s, __builtin_return_address(0));
    return p;
}

void *__wrap_memalign(size_t a, size_t n)
{
    void *p = __real_memalign(a, n);
    mt_note("memalign", p, n, __builtin_return_address(0));
    return p;
}

void *__wrap_realloc(void *old, size_t n)
{
    mt_forget(old, __builtin_return_address(0));
    void *p = __real_realloc(old, n);
    mt_note("realloc", p, n, __builtin_return_address(0));
    return p;
}

void __wrap_free(void *p)
{
    mt_forget(p, __builtin_return_address(0));
    __real_free(p);
}
#endif
