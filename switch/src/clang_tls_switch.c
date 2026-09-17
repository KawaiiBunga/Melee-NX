// TLS and stack guard shim — copied verbatim from KartPad-NX.
// Fixes two confirmed Switch crash classes:
//   1. Pipeline-worker stack overflow: libnx default 128 KB pthread stack vs
//      Dawn/Tint WGSL compiler frames (~54 KB deep). Floor raised to 8 MB.
//   2. Exit-path stack overflow: C++ fini + libnx service teardown on the ~1 MB
//      main thread. Wrapped to _exit() to skip fini.
// Both were hardware-confirmed in KartPad-NX before this shim was added.

#include <switch.h>
#include <pthread.h>
#include <stdlib.h>

#define SWITCH_MIN_PTHREAD_STACK (8 * 1024 * 1024)

typedef int (*pthread_create_fn)(pthread_t*, const pthread_attr_t*, void*(*)(void*), void*);
extern pthread_create_fn __real_pthread_create;

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start)(void*), void* arg) {
    pthread_attr_t local_attr;
    int attr_owned = 0;
    if (!attr) {
        pthread_attr_init(&local_attr);
        attr = &local_attr;
        attr_owned = 1;
    }
    size_t stack_size = 0;
    pthread_attr_getstacksize(attr, &stack_size);
    if (stack_size < SWITCH_MIN_PTHREAD_STACK) {
        if (!attr_owned) {
            pthread_attr_init(&local_attr);
            pthread_attr_t src = *attr;
            (void)src;
            attr_owned = 1;
        }
        pthread_attr_setstacksize(&local_attr, SWITCH_MIN_PTHREAD_STACK);
        attr = &local_attr;
    }
    int rc = __real_pthread_create(thread, attr, start, arg);
    if (attr_owned)
        pthread_attr_destroy(&local_attr);
    return rc;
}

void __wrap_exit(int status) {
    _exit(status);
}
