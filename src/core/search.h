#ifndef SEARCH_H
#define SEARCH_H

#include "criteria.h"
#include "../platform/platform.h"
#include "pattern.h"
#include "../platform/thread_pool.h"
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct search_result search_result_t;
typedef struct search_context search_context_t;

struct search_result {
    char *path;
    bool is_directory;
    uint64_t size;
    FILETIME mtime;
    FILETIME ctime;
    search_result_t *next;
};

typedef bool (*result_callback_t)(const search_result_t *result, void *user_data);

typedef bool (*search_progress_callback_t)(size_t processed_files, size_t queued_dirs,
                                          size_t total_results, void *user_data);

struct search_context {
    search_criteria_t *criteria;
    atomic_size_t total_results;
    atomic_size_t processed_files;
    atomic_size_t queued_dirs;
    search_result_t *results_head;
    search_result_t *results_tail;
    CRITICAL_SECTION results_lock;
    atomic_bool should_stop;

    result_callback_t result_callback;
    void *result_user_data;
    search_progress_callback_t progress_callback;
    void *progress_user_data;

    thread_pool_t *thread_pool;
};

int search_files_fast(search_criteria_t *criteria, search_result_t **results, size_t *count);

int search_files_advanced(search_criteria_t *criteria,
                         search_result_t **results, size_t *count,
                         result_callback_t result_callback, void *result_user_data,
                         search_progress_callback_t progress_callback, void *progress_user_data);

void free_search_results(search_result_t *results);
search_result_t* create_search_result(const char *path, bool is_directory, uint64_t size, FILETIME mtime, FILETIME ctime);

void search_request_cancellation(search_context_t *ctx);

bool get_last_search_thread_stats(thread_pool_stats_t *stats);

#endif
