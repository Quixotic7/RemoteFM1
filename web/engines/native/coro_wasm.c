/* SPDX-License-Identifier: GPL-3.0-only */
/* core-glue/common/coro.h on WebAssembly: the native engine's replacement for FM1VST's ucontext coro.c (selected by
 * web/engines/native/cmake/CMakeLists.txt; FM1VST is not modified). Same API, same rules: one level, the coroutine
 * runs only inside fm1coro_resume(), on the caller's thread.
 *
 * Implementation: Binaryen's Asyncify (build.sh runs wasm-opt --asyncify on the Dinghy cores, with
 * env.fm1w_co_yield as the only async import) driven by the engine's JavaScript (engine.js, "the coroutine"):
 *
 *   fm1coro_resume(c)   saves the caller's C stack pointer and calls the import fm1w_co_call(c, data, started).
 *                       JavaScript calls the export fmw_co_entry(c) as a NESTED call (a fresh wasm activation), after
 *                       asyncify_start_rewind(data) when the coroutine has run before. When that call returns, the
 *                       coroutine has yielded (JavaScript stops the unwind) or finished; the caller's stack pointer
 *                       is put back. The resumer's own frames are never unwound: only the nested activation is.
 *   fm1coro_yield()     records the coroutine's stack pointer and calls the import fm1w_co_yield(data): the first
 *                       time JavaScript starts an unwind (the nested activation returns to fm1w_co_call); when
 *                       rewound into it, JavaScript stops the rewind and the call returns: the coroutine goes on.
 *   fm1coro_park()      marks it done, then unwinds as a yield that is never rewound (exit() inside the firmware).
 *
 * The coroutine has its own C stack (malloc'ed, default FM1CORO_STACK): fmw_co_entry moves the stack pointer there
 * before the first call of fn, and every rewind puts back the pointer the yield recorded (Asyncify restores the wasm
 * locals, not the __stack_pointer global). The Asyncify buffer (its saved locals) is FM1W_CO_DATA bytes per
 * coroutine. The functions below that touch the stack pointer have no shadow-stack frame of their own. */
#include "coro.h"

#include <emscripten/stack.h>
#include <stdlib.h>
#include <string.h>

/* (Emscripten's stack_ops.S: the C stack pointer, __stack_pointer) */
void _emscripten_stack_restore(uintptr_t sp);

#define FM1W_CO_DATA (64u * 1024u)
#define FILL 0xA5u

__attribute__((import_module("env"), import_name("fm1w_co_call"))) void fm1w_co_call(void *c, void *data,
                                                                                        int started);
__attribute__((import_module("env"), import_name("fm1w_co_yield"))) void fm1w_co_yield(void *data);

struct fm1coro {
    void (*fn)(void *);
    void *arg;
    uint8_t *stack;                     /* the coroutine's C stack (grows down from stack + stack_len) */
    size_t stack_len;
    uintptr_t sp;                       /* its stack pointer at the last yield */
    int started, done;
    uint32_t resumes;
    uint32_t *data;                     /* the Asyncify buffer: {current, end}, then the saved locals */
};

static fm1coro_t *cur;                  /* the coroutine running now, or NULL */

fm1coro_t *fm1coro_new(void (*fn)(void *), void *arg, size_t stack_bytes)
{
    fm1coro_t *c;
    if (!fn)
        return NULL;
    if (!stack_bytes)
        stack_bytes = FM1CORO_STACK;
    stack_bytes = (stack_bytes + 15u) & ~(size_t)15u;
    c = (fm1coro_t *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->stack = (uint8_t *)malloc(stack_bytes);
    c->data = (uint32_t *)malloc(FM1W_CO_DATA);
    if (!c->stack || !c->data) {
        free(c->stack);
        free(c->data);
        free(c);
        return NULL;
    }
    memset(c->stack, FILL, stack_bytes);
    c->stack_len = stack_bytes;
    c->fn = fn;
    c->arg = arg;
    return c;
}

/* the Asyncify buffer emptied before an unwind (a rewind reads it as the unwind left it) */
static void fm1w_co_data_reset(fm1coro_t *c)
{
    c->data[0] = (uint32_t)(uintptr_t)(c->data + 2);
    c->data[1] = (uint32_t)((uintptr_t)c->data + FM1W_CO_DATA);
}

/* the coroutine's body, on its own stack (a frame of its own: below fmw_co_entry's switch) */
__attribute__((noinline)) static void fm1w_co_body(fm1coro_t *c)
{
    c->fn(c->arg);
    c->done = 1;
}

/* called by JavaScript (fm1w_co_call), as a nested activation: the first run, or a rewind into the last yield */
__attribute__((used, export_name("fmw_co_entry"))) void fmw_co_entry(fm1coro_t *c)
{
    if (!c->started) {
        c->started = 1;
        _emscripten_stack_restore((uintptr_t)(c->stack + c->stack_len));
    }
    fm1w_co_body(c);
}

int fm1coro_resume(fm1coro_t *c)
{
    uintptr_t back;
    if (cur)
        return -1;
    if (!c || c->done)
        return 0;
    c->resumes++;
    cur = c;
    back = emscripten_stack_get_current();
    if (c->started)
        _emscripten_stack_restore(c->sp);   /* (where the rewound frames left it) */
    fm1w_co_call(c, c->data, c->started);
    _emscripten_stack_restore(back);
    cur = NULL;
    return c->done ? 0 : 1;
}

void fm1coro_yield(void)
{
    fm1coro_t *c = cur;
    if (!c)
        return;
    c->sp = emscripten_stack_get_current();
    fm1w_co_data_reset(c);
    fm1w_co_yield(c->data);             /* (unwinds; returns here when rewound) */
    cur = c;
}

void fm1coro_park(void)
{
    fm1coro_t *c = cur;
    if (!c)
        return;
    c->done = 1;
    c->sp = emscripten_stack_get_current();
    fm1w_co_data_reset(c);
    fm1w_co_yield(c->data);             /* (unwinds for good: never rewound) */
}

int fm1coro_inside(void) { return cur != NULL; }
int fm1coro_done(const fm1coro_t *c) { return c ? c->done : 1; }
uint32_t fm1coro_resumes(const fm1coro_t *c) { return c ? c->resumes : 0; }

size_t fm1coro_stack_peak(const fm1coro_t *c)
{
    size_t i;
    if (!c || !c->started)
        return 0;
    for (i = 0; i < c->stack_len && c->stack[i] == FILL; i++)
        ;
    return c->stack_len - i;
}

void fm1coro_free(fm1coro_t *c)
{
    if (!c || c == cur)
        return;
    free(c->stack);
    free(c->data);
    free(c);
}
