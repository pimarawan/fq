#include "thread_pool.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct work_item {
    work_function_t work_func;
    void *user_data;
    struct work_item *next;
} work_item_t;

struct thread_pool {
    HANDLE *threads;
    size_t thread_count;

    CRITICAL_SECTION queue_lock;
    work_item_t *queue_head;
    work_item_t *queue_tail;
    HANDLE work_semaphore;
    HANDLE done_event;
    bool shutdown_requested;

    size_t active_work_items;
    size_t completed_work_items;
    size_t total_submitted;
    size_t queued_work_items;

    thread_pool_config_t config;
};

static DWORD WINAPI thread_pool_worker(LPVOID param) {
    thread_pool_t *pool = (thread_pool_t*)param;

    for (;;) {
        bool stopping = pool->config.stop_flag &&
                        atomic_load(pool->config.stop_flag);
        if (stopping) {
            // Stop requested. Drain any remaining queued work before exiting,
            // so wait_completion's (active==0 && queued==0) condition can
            // converge instead of spinning until --timeout. The work_func
            // (process_directory_work) has its own stop check at entry and
            // returns immediately on the cleanup path, so popping orphaned
            // items is essentially free.
            EnterCriticalSection(&pool->queue_lock);
            bool queue_empty = (pool->queue_head == NULL);
            LeaveCriticalSection(&pool->queue_lock);
            if (queue_empty) {
                break;
            }
            // Otherwise fall through to the wait below. We use a short
            // timeout (not INFINITE) when stopping because the queue may
            // be drained between our queue_empty check and the wait: if
            // another worker pops the last item between (1) and (3), the
            // semaphore signal that submit() released for that item is
            // gone and no further signal will arrive, leaving an
            // INFINITE wait permanently stuck. Polling at 50 ms lets us
            // re-check stop+queue cheaply.
        }

        DWORD wait_ms = stopping ? 50 : INFINITE;
        DWORD wait_result = WaitForSingleObject(pool->work_semaphore, wait_ms);
        if (wait_result == WAIT_TIMEOUT) {
            continue;  // re-check stop_flag and queue at loop top
        }
        if (wait_result != WAIT_OBJECT_0) {
            break;
        }

        work_item_t *item = NULL;
        EnterCriticalSection(&pool->queue_lock);
        if (pool->queue_head) {
            item = pool->queue_head;
            pool->queue_head = item->next;
            if (!pool->queue_head) {
                pool->queue_tail = NULL;
            }
            if (pool->queued_work_items > 0) {
                pool->queued_work_items--;
            }
            pool->active_work_items++;
        }
        LeaveCriticalSection(&pool->queue_lock);

        if (!item) {
            continue;
        }

        item->work_func(NULL, item->user_data);

        EnterCriticalSection(&pool->queue_lock);
        pool->completed_work_items++;
        if (pool->active_work_items > 0) {
            pool->active_work_items--;
        }

        if (pool->shutdown_requested && pool->active_work_items == 0 && pool->queued_work_items == 0) {
            SetEvent(pool->done_event);
        }
        LeaveCriticalSection(&pool->queue_lock);

        free(item);
    }

    return 0;
}

thread_pool_t* thread_pool_create(const thread_pool_config_t *config) {
    if (!config) return NULL;

    thread_pool_t *pool = (thread_pool_t*)calloc(1, sizeof(thread_pool_t));
    if (!pool) return NULL;

    pool->config = *config;

    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    size_t hw_threads = sysinfo.dwNumberOfProcessors > 0 ? sysinfo.dwNumberOfProcessors : 4;
    pool->thread_count = config->max_threads > 0 ? config->max_threads : hw_threads;

    pool->active_work_items = 0;
    pool->completed_work_items = 0;
    pool->total_submitted = 0;
    pool->queued_work_items = 0;

    InitializeCriticalSection(&pool->queue_lock);
    pool->work_semaphore = CreateSemaphoreA(NULL, 0, LONG_MAX, NULL);
    pool->done_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!pool->work_semaphore || !pool->done_event) {
        if (pool->work_semaphore) CloseHandle(pool->work_semaphore);
        if (pool->done_event) CloseHandle(pool->done_event);
        DeleteCriticalSection(&pool->queue_lock);
        free(pool);
        return NULL;
    }

    pool->threads = (HANDLE*)calloc(pool->thread_count, sizeof(HANDLE));
    if (!pool->threads) {
        CloseHandle(pool->work_semaphore);
        DeleteCriticalSection(&pool->queue_lock);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
        pool->threads[i] = CreateThread(NULL, 0, thread_pool_worker, pool, 0, NULL);
        if (!pool->threads[i]) {
            pool->thread_count = i;
            break;
        }
    }

    if (pool->thread_count == 0) {
        CloseHandle(pool->work_semaphore);
        DeleteCriticalSection(&pool->queue_lock);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    return pool;
}

bool thread_pool_submit(thread_pool_t *pool, work_function_t work_func, void *user_data) {
    if (!pool || !work_func) return false;

    if (pool->config.stop_flag && atomic_load(pool->config.stop_flag)) {
        return false;
    }

    work_item_t *item = (work_item_t*)malloc(sizeof(work_item_t));
    if (!item) return false;

    item->work_func = work_func;
    item->user_data = user_data;
    item->next = NULL;

    EnterCriticalSection(&pool->queue_lock);
    if (pool->queue_tail) {
        pool->queue_tail->next = item;
        pool->queue_tail = item;
    } else {
        pool->queue_head = pool->queue_tail = item;
    }
    pool->queued_work_items++;
    pool->total_submitted++;
    // new work means completion not reached
    ResetEvent(pool->done_event);
    LeaveCriticalSection(&pool->queue_lock);

    ReleaseSemaphore(pool->work_semaphore, 1, NULL);

    return true;
}

bool thread_pool_wait_completion(thread_pool_t *pool, DWORD timeout_ms) {
    if (!pool) return false;

    EnterCriticalSection(&pool->queue_lock);
    pool->shutdown_requested = true;
    ResetEvent(pool->done_event);
    bool already_done = (pool->active_work_items == 0 && pool->queued_work_items == 0);
    LeaveCriticalSection(&pool->queue_lock);

    if (already_done) {
        return true;
    }

    DWORD start = GetTickCount();
    for (;;) {
        size_t active, queued, completed;
        EnterCriticalSection(&pool->queue_lock);
        active = pool->active_work_items;
        queued = pool->queued_work_items;
        completed = pool->completed_work_items;
        LeaveCriticalSection(&pool->queue_lock);

        if (active == 0 && queued == 0) {
            return true;
        }

        if (timeout_ms != INFINITE) {
            DWORD elapsed = GetTickCount() - start;
            if (elapsed >= timeout_ms) {
                return false;
            }
        }

        if (pool->config.progress_cb) {
            if (!pool->config.progress_cb(completed, active, pool->config.progress_user_data)) {
                if (pool->config.stop_flag) {
                    atomic_store(pool->config.stop_flag, true);
                }
                return false;
            }
        }

        DWORD wait_slice = (timeout_ms == INFINITE) ? 50 : (timeout_ms < 50 ? timeout_ms : 50);
        DWORD wait_result = WaitForSingleObject(pool->done_event, wait_slice);
        if (wait_result == WAIT_OBJECT_0) {
            // signaled; loop will re-check counts
            continue;
        }
    }
}

void thread_pool_destroy(thread_pool_t *pool) {
    if (!pool) return;

    if (pool->config.stop_flag) {
        atomic_store(pool->config.stop_flag, true);
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
        ReleaseSemaphore(pool->work_semaphore, 1, NULL);
    }

    WaitForMultipleObjects((DWORD)pool->thread_count, pool->threads, TRUE, 5000);

    for (size_t i = 0; i < pool->thread_count; i++) {
        if (pool->threads[i]) {
            CloseHandle(pool->threads[i]);
        }
    }

    CloseHandle(pool->work_semaphore);
    CloseHandle(pool->done_event);
    DeleteCriticalSection(&pool->queue_lock);

    work_item_t *item = pool->queue_head;
    while (item) {
        work_item_t *next = item->next;
        free(item);
        item = next;
    }

    free(pool->threads);
    free(pool);
}

bool thread_pool_get_stats(thread_pool_t *pool, thread_pool_stats_t *stats) {
    if (!pool || !stats) return false;

    EnterCriticalSection(&pool->queue_lock);
    stats->active_threads = pool->active_work_items;
    stats->queued_work_items = pool->queued_work_items;
    stats->completed_work_items = pool->completed_work_items;
    stats->total_submitted = pool->total_submitted;
    LeaveCriticalSection(&pool->queue_lock);

    return true;
}
