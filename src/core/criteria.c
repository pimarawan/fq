#include "criteria.h"
#include "../platform/platform.h"
#include "../util/utils.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void criteria_init(search_criteria_t *criteria) {
    if (!criteria) return;

    memset(criteria, 0, sizeof(*criteria));

    criteria->case_sensitive = false;
    criteria->use_glob = false;
    criteria->use_regex = false;
    criteria->skip_common_dirs = true;
    criteria->preview_mode = false;
    criteria->preview_lines = 10;
    criteria->file_type_filter = NULL;
    criteria->max_threads = 0;
    criteria->timeout_ms = 300000;    // 5 minutes
    criteria->follow_symlinks = false;
    criteria->include_hidden = false;
    criteria->max_results = 0;        // Unlimited
    criteria->max_depth = SIZE_MAX;
    criteria->include_directories = false;
    criteria->include_files = true;
}

bool criteria_parse_extensions(search_criteria_t *criteria, const char *extensions_str) {
    if (!criteria) return false;

    if (criteria->extensions) {
        for (size_t i = 0; i < criteria->extensions_count; i++) {
            free(criteria->extensions[i]);
        }
        free(criteria->extensions);
        criteria->extensions = NULL;
        criteria->extensions_count = 0;
    }

    if (!extensions_str || *extensions_str == '\0') {
        return true;
    }

    size_t count = 1;
    for (const char *p = extensions_str; *p; p++) {
        if (*p == ',') count++;
    }

    criteria->extensions = malloc(count * sizeof(char*));
    if (!criteria->extensions) return false;

    char *copy = _strdup(extensions_str);
    if (!copy) {
        free(criteria->extensions);
        criteria->extensions = NULL;
        return false;
    }

    char *token = strtok(copy, ",");
    size_t index = 0;

    while (token && index < count) {
        while (isspace(*token)) token++;
        char *end = token + strlen(token) - 1;
        while (end > token && isspace(*end)) *end-- = '\0';

        if (*token == '.') token++;

        if (*token != '\0') {
            criteria->extensions[index] = _strdup(token);
            if (criteria->extensions[index]) {
                CharLowerBuffA(criteria->extensions[index], (DWORD)strlen(criteria->extensions[index]));
                index++;
            }
        }

        token = strtok(NULL, ",");
    }

    criteria->extensions_count = index;
    free(copy);

    // Shrink array if parsed fewer extensions than expected
    if (index < count) {
        char **new_array = realloc(criteria->extensions, index * sizeof(char*));
        if (new_array || index == 0) {
            criteria->extensions = new_array;
        }
    }

    return true;
}

void criteria_cleanup(search_criteria_t *criteria) {
    if (!criteria) return;

    free(criteria->root_path);
    free(criteria->search_term);
    free(criteria->file_type_filter);

    if (criteria->extensions) {
        for (size_t i = 0; i < criteria->extensions_count; i++) {
            free(criteria->extensions[i]);
        }
        free(criteria->extensions);
    }

    memset(criteria, 0, sizeof(*criteria));
}

bool criteria_validate(const search_criteria_t *criteria) {
    if (!criteria) return false;

    if (!criteria->root_path || *criteria->root_path == '\0') {
        return false;
    }

    if (!criteria->include_files && !criteria->include_directories) {
        return false;
    }

    if (criteria->has_min_size && criteria->has_max_size &&
        criteria->min_size > criteria->max_size) {
        return false;
    }

    if (criteria->has_exact_size && (criteria->has_min_size || criteria->has_max_size)) {
        return false;
    }

    if (criteria->has_after_time && criteria->has_before_time &&
        CompareFileTime(&criteria->after_time, &criteria->before_time) > 0) {
        return false;
    }

    if (criteria->has_after_ctime && criteria->has_before_ctime &&
        CompareFileTime(&criteria->after_ctime, &criteria->before_ctime) > 0) {
        return false;
    }

    return true;
}

bool criteria_extension_matches(const char *filename, const search_criteria_t *criteria) {
    if (!filename || !criteria || criteria->extensions_count == 0) {
        return criteria->extensions_count == 0;
    }

    const char *ext = strrchr(filename, '.');
    if (!ext) return false;

    ext++;
    if (*ext == '\0') return false;

    char ext_lower[64];
    size_t ext_len = strlen(ext);
    if (ext_len >= sizeof(ext_lower)) return false;

    for (size_t i = 0; i <= ext_len; i++) {
        ext_lower[i] = (char)tolower(ext[i]);
    }

    for (size_t i = 0; i < criteria->extensions_count; i++) {
        if (strcmp(ext_lower, criteria->extensions[i]) == 0) {
            return true;
        }
    }

    return false;
}

bool criteria_size_matches(uint64_t file_size, const search_criteria_t *criteria) {
    if (!criteria) return true;

    if (criteria->has_exact_size && file_size != criteria->exact_size) {
        return false;
    }

    if (criteria->has_min_size && file_size < criteria->min_size) {
        return false;
    }

    if (criteria->has_max_size && file_size > criteria->max_size) {
        return false;
    }

    return true;
}

bool criteria_time_matches(const FILETIME *file_time, const search_criteria_t *criteria) {
    if (!file_time || !criteria) return true;

    if (criteria->has_after_time &&
        CompareFileTime(file_time, &criteria->after_time) < 0) {
        return false;
    }

    if (criteria->has_before_time &&
        CompareFileTime(file_time, &criteria->before_time) > 0) {
        return false;
    }

    return true;
}

bool criteria_ctime_matches(const FILETIME *file_time, const search_criteria_t *criteria) {
    if (!file_time || !criteria) return true;

    if (criteria->has_after_ctime &&
        CompareFileTime(file_time, &criteria->after_ctime) < 0) {
        return false;
    }

    if (criteria->has_before_ctime &&
        CompareFileTime(file_time, &criteria->before_ctime) > 0) {
        return false;
    }

    return true;
}

bool criteria_file_type_matches(const char *filename, const search_criteria_t *criteria) {
    if (!filename || !criteria || !criteria->file_type_filter) {
        return true;
    }

    const char *filter = criteria->file_type_filter;

    if (_stricmp(filter, "text") == 0) {
        return has_extension(filename, text_extensions);
    }
    if (_stricmp(filter, "image") == 0) {
        return has_extension(filename, image_extensions);
    }
    if (_stricmp(filter, "video") == 0) {
        return has_extension(filename, video_extensions);
    }
    if (_stricmp(filter, "audio") == 0) {
        return has_extension(filename, audio_extensions);
    }
    if (_stricmp(filter, "archive") == 0) {
        return has_extension(filename, archive_extensions);
    }

    return false;
}
