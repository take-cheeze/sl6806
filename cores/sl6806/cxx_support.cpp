/*
 * cxx_support.cpp - the small amount of C++ runtime a freestanding build needs.
 *
 * new/delete go to malloc so String and friends work. Exceptions and RTTI are
 * off (see the build flags), so nothing here needs an unwinder.
 */

#include <stdlib.h>
#include <stddef.h>

void *operator new(size_t size)                 { return malloc(size); }
void *operator new[](size_t size)               { return malloc(size); }
void *operator new(size_t, void *p)   noexcept  { return p; }
void *operator new[](size_t, void *p) noexcept  { return p; }

void operator delete(void *p)                   noexcept { free(p); }
void operator delete[](void *p)                 noexcept { free(p); }
void operator delete(void *p, size_t)           noexcept { free(p); }
void operator delete[](void *p, size_t)         noexcept { free(p); }

extern "C" {

/* Called if a pure virtual somehow gets invoked - a bug, so say so loudly
 * rather than running off into undefined behaviour. */
void __cxa_pure_virtual(void);
void __cxa_pure_virtual(void)
{
    extern void sl6806_panic(const char *) __attribute__((noreturn));
    sl6806_panic("pure virtual function called");
}

/* Guards for function-local statics. Single-threaded, so these are trivial. */
int  __cxa_guard_acquire(__UINT64_TYPE__ *g) { return !*(char *)g; }
void __cxa_guard_release(__UINT64_TYPE__ *g) { *(char *)g = 1; }
void __cxa_guard_abort(__UINT64_TYPE__ *g)   { (void)g; }

/* Static destructors never run - the sketch never exits. */
int  __cxa_atexit(void (*f)(void *), void *a, void *d) { (void)f; (void)a; (void)d; return 0; }
void *__dso_handle = 0;

}
