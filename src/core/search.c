#include "search.h"
#include "../util/utils.h"
#include "pattern.h"
#include "../platform/platform.h"
#include "../platform/thread_pool.h"
#include "criteria.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static thread_pool_stats_t last_thread_stats = {0};
static bool last_thread_stats_valid = false;

typedef struct {
    search_context_t *ctx;
    char *directory_path;
    size_t depth;
} directory_work_t;

static const char* skip_directories[] = {
    "$RECYCLE.BIN", "System Volume Information", "Windows", "Program Files",
    "Program Files (x86)", "ProgramData", "Recovery", "Intel", "AMD", "NVIDIA",
    "node_modules", ".git", ".svn", "__pycache__", "obj", "bin", "Debug",
    "Release", ".vs", "packages", "bower_components", "dist", "build"
};

static bool component_equals(const char *component, const char *name) {
    return _stricmp(component, name) == 0;
}

static bool is_system_directory(const char *path) {
    if (!path) return false;

    // Skip drive letter
    const char *p = path;
    if (p[1] == ':') {
        p += 2;
    }

    // Skip leading separators
    while (*p == '\\' || *p == '/') {
        p++;
    }

    bool saw_windows = false;

    while (*p) {
        const char *start = p;
        while (*p && *p != '\\' && *p != '/') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len > 0) {
            char component[256];
            if (len >= sizeof(component)) {
                len = sizeof(component) - 1;
            }
            memcpy(component, start, len);
            component[len] = '\0';

            if (component_equals(component, "windows")) {
                saw_windows = true;
            } else {
                if (saw_windows && (component_equals(component, "system32") || component_equals(component, "syswow64"))) {
                    return true;
                }
                saw_windows = false;
            }

            if (component_equals(component, "$recycle.bin") ||
                component_equals(component, "system volume information") ||
                component_equals(component, "program files") ||
                component_equals(component, "program files (x86)") ||
                component_equals(component, "programdata") ||
                component_equals(component, "recovery") ||
                component_equals(component, "intel") ||
                component_equals(component, "amd") ||
                component_equals(component, "nvidia") ||
                component_equals(component, "hiberfil.sys") ||
                component_equals(component, "pagefile.sys") ||
                component_equals(component, "swapfile.sys")) {
                return true;
            }
        }

        if (*p == '\\' || *p == '/') {
            p++;
        }
    }

    return saw_windows; // unlikely, but if path ends with "windows" treat as system
}

static bool should_skip_directory(const char *dirname, const search_criteria_t *criteria) {
    if (!dirname || !criteria || !criteria->skip_common_dirs) return false;

    for (size_t i = 0; i < sizeof(skip_directories) / sizeof(skip_directories[0]); i++) {
        if (_stricmp(dirname, skip_directories[i]) == 0) return true;
    }
    return false;
}

search_result_t* create_search_result(const char *path, bool is_directory, uint64_t size, FILETIME mtime, FILETIME ctime) {
    if (!path) return NULL;

    search_result_t *result = malloc(sizeof(search_result_t));
    if (!result) return NULL;

    result->path = _strdup(path);
    if (!result->path) {
        free(result);
        return NULL;
    }
    for (char *p = result->path; *p; ++p) {
        if (*p == '/') {
            *p = '\\';
        }
    }

    result->is_directory = is_directory;
    result->size = size;
    result->mtime = mtime;
    result->ctime = ctime;
    result->next = NULL;

    return result;
}

static bool add_result_safe(search_context_t *ctx, const char *path, bool is_directory, uint64_t size, FILETIME mtime, FILETIME ctime) {
    if (!ctx || !path) return false;

    if (atomic_load(&ctx->should_stop)) {
        return false;
    }

    if (ctx->criteria->max_results > 0 &&
        atomic_load(&ctx->total_results) >= ctx->criteria->max_results) {
        atomic_store(&ctx->should_stop, true);
        return false;
    }

    search_result_t *result = create_search_result(path, is_directory, size, mtime, ctime);
    if (!result) return false;

    bool continue_search = true;
    EnterCriticalSection(&ctx->results_lock);
    if (ctx->result_callback) {
        continue_search = ctx->result_callback(result, ctx->result_user_data);
        if (!continue_search) {
            atomic_store(&ctx->should_stop, true);
        }
    }

    if (!ctx->results_head) {
        ctx->results_head = result;
        ctx->results_tail = result;
    } else {
        ctx->results_tail->next = result;
        ctx->results_tail = result;
    }
    atomic_fetch_add(&ctx->total_results, 1);
    LeaveCriticalSection(&ctx->results_lock);

    if (ctx->criteria->max_results > 0 &&
        atomic_load(&ctx->total_results) >= ctx->criteria->max_results) {
        atomic_store(&ctx->should_stop, true);
    }

    return continue_search;
}

static bool matches_file_criteria(const platform_file_info_t *file_info, const search_criteria_t *criteria) {
    if (!file_info || !criteria) return false;

    if (!criteria_size_matches(file_info->size, criteria)) return false;
    if (!criteria_time_matches(&file_info->mtime, criteria)) return false;
    if (!criteria_ctime_matches(&file_info->ctime, criteria)) return false;
    if (!criteria_extension_matches(file_info->name, criteria)) return false;
    if (!criteria_file_type_matches(file_info->name, criteria)) return false;

    if (criteria->search_term && *criteria->search_term) {
        if (!pattern_matches(file_info->name, criteria->search_term,
                             criteria->case_sensitive, criteria->use_glob, criteria->use_regex)) {
            return false;
        }
    }

    return true;
}

static bool matches_directory_criteria(const platform_file_info_t *file_info, const search_criteria_t *criteria) {
    if (!file_info || !criteria) return false;

    if (!criteria_time_matches(&file_info->mtime, criteria)) return false;
    if (!criteria_ctime_matches(&file_info->ctime, criteria)) return false;

    if (criteria->search_term && *criteria->search_term) {
        if (!pattern_matches(file_info->name, criteria->search_term,
                             criteria->case_sensitive, criteria->use_glob, criteria->use_regex)) {
            return false;
        }
    }

    return true;
}

static void process_directory_work(void *context, void *user_data) {
    (void)context;

    directory_work_t *work = (directory_work_t*)user_data;
    search_context_t *ctx = work->ctx;

    if (atomic_load(&ctx->should_stop)) {
        goto cleanup;
    }

    if (is_system_directory(work->directory_path)) {
        goto cleanup;
    }

    platform_dir_iter_t *dir_iter = platform_opendir(work->directory_path);
    if (!dir_iter) {
        goto cleanup;
    }

    platform_file_info_t file_info;
    while (platform_readdir(dir_iter, &file_info)) {
        if (atomic_load(&ctx->should_stop)) {
            platform_free_file_info(&file_info);
            break;
        }

        if (!ctx->criteria->include_hidden && file_info.name[0] == '.') {
            platform_free_file_info(&file_info);
            continue;
        }

        char full_path[MAX_PATH * 2];
        int written = snprintf(full_path, sizeof(full_path), "%s\\%s", work->directory_path, file_info.name);
        if (written < 0 || (size_t)written >= sizeof(full_path)) {
            platform_free_file_info(&file_info);
            continue;
        }

        if (file_info.is_directory) {
            if (file_info.is_symlink && !ctx->criteria->follow_symlinks) {
                platform_free_file_info(&file_info);
                continue;
            }

            if (!should_skip_directory(file_info.name, ctx->criteria)) {
                if (ctx->criteria->include_directories && matches_directory_criteria(&file_info, ctx->criteria)) {
                    add_result_safe(ctx, full_path, true, 0, file_info.mtime, file_info.ctime);
                }

                // Check depth limit before recursing into subdirectory
                // max_depth == 0 means current directory only (no recursion)
                // work->depth starts at 0, so depth 1+ directories require max_depth >= 1
                if (work->depth < ctx->criteria->max_depth) {
                    directory_work_t *subdir_work = malloc(sizeof(directory_work_t));
                    if (subdir_work) {
                        subdir_work->ctx = ctx;
                        subdir_work->directory_path = _strdup(full_path);
                        subdir_work->depth = work->depth + 1;

                        if (subdir_work->directory_path) {
                            atomic_fetch_add(&ctx->queued_dirs, 1);
                            if (!thread_pool_submit(ctx->thread_pool, process_directory_work, subdir_work)) {
                                process_directory_work(NULL, subdir_work);
                            }
                        } else {
                            free(subdir_work);
                        }
                    }
                }
            }
        } else {
            if (ctx->criteria->include_files && matches_file_criteria(&file_info, ctx->criteria)) {
                add_result_safe(ctx, full_path, false, file_info.size, file_info.mtime, file_info.ctime);
            }
            atomic_fetch_add(&ctx->processed_files, 1);
        }

        platform_free_file_info(&file_info);
    }

    platform_closedir(dir_iter);

cleanup:
    free(work->directory_path);
    free(work);
    atomic_fetch_sub(&ctx->queued_dirs, 1);
}

static bool search_progress_callback(size_t processed_files, size_t queued_dirs, void *user_data) {
    search_context_t *ctx = (search_context_t*)user_data;
    (void)processed_files;
    (void)queued_dirs;

    if (ctx->thread_pool) {
        thread_pool_get_stats(ctx->thread_pool, &last_thread_stats);
        last_thread_stats_valid = true;
    }

    if (ctx->progress_callback) {
        size_t files_processed = atomic_load(&ctx->processed_files);
        size_t dirs_pending = atomic_load(&ctx->queued_dirs);
        size_t total_results = atomic_load(&ctx->total_results);
        return ctx->progress_callback(files_processed, dirs_pending, total_results, ctx->progress_user_data);
    }

    return !atomic_load(&ctx->should_stop);
}

int search_files_advanced(search_criteria_t *criteria,
                         search_result_t **results, size_t *count,
                         result_callback_t result_callback, void *result_user_data,
                         search_progress_callback_t progress_callback, void *progress_user_data) {
    if (!criteria || !criteria_validate(criteria)) return -1;

    if (results) *results = NULL;
    if (count) *count = 0;

    search_context_t ctx = {0};
    ctx.criteria = criteria;
    atomic_init(&ctx.total_results, 0);
    atomic_init(&ctx.processed_files, 0);
    atomic_init(&ctx.queued_dirs, 0);
    atomic_init(&ctx.should_stop, false);
    ctx.results_head = NULL;
    ctx.results_tail = NULL;
    ctx.result_callback = result_callback;
    ctx.result_user_data = result_user_data;
    ctx.progress_callback = progress_callback;
    ctx.progress_user_data = progress_user_data;

    if (!InitializeCriticalSectionAndSpinCount(&ctx.results_lock, 4000)) {
        return -1;
    }

    thread_pool_config_t pool_config = {0};
    pool_config.max_threads = criteria->max_threads;
    pool_config.progress_cb = search_progress_callback;
    pool_config.progress_user_data = &ctx;
    pool_config.stop_flag = &ctx.should_stop;

    ctx.thread_pool = thread_pool_create(&pool_config);
    if (!ctx.thread_pool) {
        DeleteCriticalSection(&ctx.results_lock);
        return -1;
    }

    directory_work_t *initial_work = malloc(sizeof(directory_work_t));
    if (!initial_work) {
        thread_pool_destroy(ctx.thread_pool);
        DeleteCriticalSection(&ctx.results_lock);
        return -1;
    }

    initial_work->ctx = &ctx;
    initial_work->directory_path = _strdup(criteria->root_path);
    initial_work->depth = 0;

    if (!initial_work->directory_path) {
        free(initial_work);
        thread_pool_destroy(ctx.thread_pool);
        DeleteCriticalSection(&ctx.results_lock);
        return -1;
    }

    atomic_fetch_add(&ctx.queued_dirs, 1);
    if (!thread_pool_submit(ctx.thread_pool, process_directory_work, initial_work)) {
        process_directory_work(NULL, initial_work);
    }

    bool completed = thread_pool_wait_completion(ctx.thread_pool, criteria->timeout_ms);
    if (!completed) {
        atomic_store(&ctx.should_stop, true);
        thread_pool_wait_completion(ctx.thread_pool, 5000);
    }

    last_thread_stats_valid = thread_pool_get_stats(ctx.thread_pool, &last_thread_stats);

    thread_pool_destroy(ctx.thread_pool);
    DeleteCriticalSection(&ctx.results_lock);

    if (results) *results = ctx.results_head;
    if (count) *count = atomic_load(&ctx.total_results);

    return completed ? 0 : -2;
}

int search_files_fast(search_criteria_t *criteria, search_result_t **results, size_t *count) {
    return search_files_advanced(criteria, results, count, NULL, NULL, NULL, NULL);
}

void search_request_cancellation(search_context_t *ctx) {
    if (ctx) {
        atomic_store(&ctx->should_stop, true);
    }
}

void free_search_results(search_result_t *results) {
    while (results) {
        search_result_t *next = results->next;
        free(results->path);
        free(results);
        results = next;
    }
}

bool get_last_search_thread_stats(thread_pool_stats_t *stats) {
    if (!stats || !last_thread_stats_valid) {
        return false;
    }
    *stats = last_thread_stats;
    return true;
}
