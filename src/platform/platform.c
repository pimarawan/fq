#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

int utf8_to_wide(const char *utf8_str, wchar_t **wide_str) {
    if (!utf8_str || !wide_str) return -1;

    int len = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
    if (len <= 0) return -1;

    *wide_str = malloc(len * sizeof(wchar_t));
    if (!*wide_str) return -1;

    int result = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, *wide_str, len);
    if (result <= 0) {
        free(*wide_str);
        *wide_str = NULL;
        return -1;
    }

    return len;
}

int wide_to_utf8(const wchar_t *wide_str, char **utf8_str) {
    if (!wide_str || !utf8_str) return -1;

    int len = WideCharToMultiByte(CP_UTF8, 0, wide_str, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return -1;

    *utf8_str = malloc(len);
    if (!*utf8_str) return -1;

    int result = WideCharToMultiByte(CP_UTF8, 0, wide_str, -1, *utf8_str, len, NULL, NULL);
    if (result <= 0) {
        free(*utf8_str);
        *utf8_str = NULL;
        return -1;
    }

    return len;
}

void free_converted_string(void *str) {
    free(str);
}

HRESULT make_long_path(const char *path, wchar_t **long_path) {
    if (!path || !long_path) return E_INVALIDARG;

    wchar_t *wide_path;
    if (utf8_to_wide(path, &wide_path) < 0) return E_FAIL;

    size_t path_len = wcslen(wide_path);
    bool needs_prefix = path_len >= MAX_PATH && wcsncmp(wide_path, L"\\\\?\\", 4) != 0;

    if (needs_prefix) {
        size_t long_path_len = path_len + 5;
        *long_path = malloc(long_path_len * sizeof(wchar_t));
        if (!*long_path) {
            free(wide_path);
            return E_OUTOFMEMORY;
        }

        wcscpy(*long_path, L"\\\\?\\");
        wcsncat(*long_path, wide_path, long_path_len - 5);

        free(wide_path);
        return S_OK;
    } else {
        *long_path = wide_path;
        return S_OK;
    }
}

struct platform_dir_iter {
    HANDLE find_handle;
    WIN32_FIND_DATAW find_data;
    bool first_call;
    wchar_t *search_pattern;
};

platform_dir_iter_t* platform_opendir(const char *utf8_path) {
    if (!utf8_path) return NULL;

    wchar_t *wide_path;
    if (FAILED(make_long_path(utf8_path, &wide_path))) {
        return NULL;
    }

    // Check if directory exists
    DWORD attrs = GetFileAttributesW(wide_path);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        free(wide_path);
        return NULL;
    }

    size_t pattern_len = wcslen(wide_path) + 3; // path + "\*" + null
    wchar_t *search_pattern = malloc(pattern_len * sizeof(wchar_t));
    if (!search_pattern) {
        free(wide_path);
        return NULL;
    }

    wcsncpy(search_pattern, wide_path, pattern_len - 1);
    search_pattern[pattern_len - 1] = L'\0';
    wcsncat(search_pattern, L"\\*", pattern_len - wcslen(search_pattern) - 1);

    free(wide_path);

    platform_dir_iter_t *iter = malloc(sizeof(platform_dir_iter_t));
    if (!iter) {
        free(search_pattern);
        return NULL;
    }

    iter->search_pattern = search_pattern;
    iter->find_handle = INVALID_HANDLE_VALUE;
    iter->first_call = true;

    return iter;
}

bool platform_readdir(platform_dir_iter_t *iter, platform_file_info_t *info) {
    if (!iter || !info) return false;

    BOOL found;
    if (iter->first_call) {
        iter->find_handle = FindFirstFileW(iter->search_pattern, &iter->find_data);
        iter->first_call = false;
        found = (iter->find_handle != INVALID_HANDLE_VALUE);
    } else {
        found = FindNextFileW(iter->find_handle, &iter->find_data);
    }

    if (!found) return false;

    if (wcscmp(iter->find_data.cFileName, L".") == 0 ||
        wcscmp(iter->find_data.cFileName, L"..") == 0) {
        return platform_readdir(iter, info);
    }

    if (wide_to_utf8(iter->find_data.cFileName, &info->name) < 0) {
        return false;
    }

    size_t name_len = wcslen(iter->find_data.cFileName) + 1;
    info->name_wide = malloc(name_len * sizeof(wchar_t));
    if (info->name_wide) {
        wcsncpy(info->name_wide, iter->find_data.cFileName, name_len - 1);
        info->name_wide[name_len - 1] = L'\0';
    }

    info->size = ((uint64_t)iter->find_data.nFileSizeHigh << 32) | iter->find_data.nFileSizeLow;
    info->mtime = iter->find_data.ftLastWriteTime;
    info->ctime = iter->find_data.ftCreationTime;
    info->is_directory = (iter->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info->is_symlink = (iter->find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    return true;
}

void platform_closedir(platform_dir_iter_t *iter) {
    if (!iter) return;

    if (iter->find_handle != INVALID_HANDLE_VALUE) {
        FindClose(iter->find_handle);
    }

    free(iter->search_pattern);
    free(iter);
}

void platform_free_file_info(platform_file_info_t *info) {
    if (!info) return;

    free(info->name);
    free(info->name_wide);
    memset(info, 0, sizeof(*info));
}
