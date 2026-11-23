/* Exercise the real notification primitive, including check-to-sleep races. */
#include "kernel/xbox_memory_layout.h"
#include "platform/win32_compat.h"
#include <stdio.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

static HANDLE ready;
static volatile LONG count, woken;
static uint32_t epoch;
static DWORD WINAPI waiter(void *unused)
{
    (void)unused;
    if (InterlockedIncrement(&count) == 2) SetEvent(ready);
    if (xbox_Nv2aProgressWait(epoch, 1000)) InterlockedIncrement(&woken);
    return 0;
}

int main(void)
{
    HANDLE threads[2];
    LARGE_INTEGER a, b, frequency;
    unsigned i;
    int failures = 0;
    QueryPerformanceFrequency(&frequency);
    /* A completion after the predicate but before parking is remembered. */
    for (i = 0; i < 100; i++) {
        epoch = xbox_Nv2aProgressEpoch();
        xbox_Nv2aProgressNotify();
        if (!xbox_Nv2aProgressWait(epoch, 100)) failures++;
    }
    epoch = xbox_Nv2aProgressEpoch();
    QueryPerformanceCounter(&a);
    if (xbox_Nv2aProgressWait(epoch, 10)) failures++;
    QueryPerformanceCounter(&b);
    if ((double)(b.QuadPart - a.QuadPart) / frequency.QuadPart > 0.5 ||
        (double)(b.QuadPart - a.QuadPart) / frequency.QuadPart < 0.005) failures++;
    ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    threads[0] = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    if (WaitForSingleObject(ready, 1000) != WAIT_OBJECT_0) failures++;
    Sleep(10);
    xbox_Nv2aProgressNotify();
    for (i = 0; i < 2; i++) {
        if (WaitForSingleObject(threads[i], 2000) != WAIT_OBJECT_0) failures++;
        CloseHandle(threads[i]);
    }
    if (woken != 2) failures++;
    CloseHandle(ready);
    printf("nv2a_progress: %d FAILURE(S)\n", failures);
    return failures ? 1 : 0;
}
