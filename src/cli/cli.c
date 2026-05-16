#include "cli.h"
#include "../core/criteria.h"
#include "../util/utils.h"
#include "../output/output.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void init_options(cli_options_t *options) {
    memset(options, 0, sizeof(cli_options_t));
    options->color_mode = COLOR_AUTO;
}

static int parse_date_arg(const char *arg, FILETIME *file_time) {
    return parse_date_string(arg, file_time);
}

void print_usage(const char *program_name) {
    printf("fq - fast file and folder search tool for Windows\n\n");
    printf("Usage: %s [pattern] [path] [OPTIONS]\n\n", program_name);

    printf("Arguments:\n");
    printf("  [pattern]           Search pattern (default: match all)\n");
    printf("  [path]              Directory to search (default: current directory)\n\n");

    printf("Search Options:\n");
    printf("  -c, --case              Case-sensitive search\n");
    printf("  -g, --glob              Enable glob patterns (* ? [] {})\n");
    printf("  -r, --regex             Enable regex patterns (name matching)\n");
    printf("  -H, --include-hidden    Include hidden files and directories\n");
    printf("  -L, --follow-symlinks   Follow symbolic links\n");
    printf("      --folders           Include folders in results\n");
    printf("      --folders-only      Return only folders (no files)\n");
    printf("  -q, --quiet             Suppress progress/summary output\n");
    printf("      --no-skip           Don't skip common directories (node_modules, .git, etc.)\n\n");
    printf("      --color <when>      Color output: auto|always|never\n\n");

    printf("Filters:\n");
    printf("  -e, --ext <list>    Filter by file extensions (comma-separated)\n");
    printf("  -t, --type <type>   Filter by file type (text, image, video, audio, archive)\n");
    printf("      --min <size>    Minimum file size (supports K, M, G, T suffixes)\n");
    printf("      --max <size>    Maximum file size (supports K, M, G, T suffixes)\n");
    printf("      --size <size>   Exact file size, or +size (larger), -size (smaller)\n");
    printf("      --after <date>  Files modified after date (YYYY-MM-DD)\n");
    printf("      --before <date> Files modified before date (YYYY-MM-DD)\n");
    printf("      --created-after <date>   Files created after date (YYYY-MM-DD)\n");
    printf("      --created-before <date>  Files created before date (YYYY-MM-DD)\n");
    printf("  -d, --max-depth <n> Maximum recursion depth (0 = no recursion, default = unlimited)\n");
    printf("      --max-results <n>   Maximum number of results (0 = unlimited)\n\n");

    printf("Performance:\n");
    printf("  -j, --threads <n>   Number of worker threads (0 = auto)\n");
    printf("      --timeout <ms>  Search timeout in milliseconds\n");
    printf("      --stats         Show real-time thread pool statistics\n\n");

    printf("Output:\n");
    printf("      --preview [<n>]     Show preview of text files (default: 10 lines)\n");
    printf("      --out <file>        Write output to file\n");
    printf("      --json              Output results as JSON\n\n");

    printf("General:\n");
    printf("  -h, --help          Show this help message\n");
    printf("  -V, --version       Show version information\n\n");

    printf("Examples:\n");
    printf("  List all files in current directory:\n");
    printf("    %s\n\n", program_name);
    printf("  Search for files matching 'main':\n");
    printf("    %s main\n\n", program_name);
    printf("  Search for all PNG files:\n");
    printf("    %s \"*.png\" D:\\ --glob\n\n", program_name);
    printf("  Find documents larger than 1MB:\n");
    printf("    %s document . --min 1M --ext pdf,docx\n\n", program_name);
    printf("  Case-sensitive search with thread monitoring:\n");
    printf("    %s Config C:\\ --case --stats --threads 8\n\n", program_name);

    printf("For glob patterns: * (any chars), ? (single char), [abc] (char set), {jpg,png} (alternatives)\n");
}

void print_version(void) {
    printf("%s\n", get_version_string());
    printf("Copyright (c) 2025. Open source under MIT license.\n");
}

static bool is_option(const char *arg) {
    return arg && arg[0] == '-';
}

static bool is_path(const char *arg) {
    if (!arg) return false;
    if (arg[0] == '.' || arg[0] == '/' || arg[0] == '\\') return true;
    if (strlen(arg) >= 2 && arg[1] == ':') return true;
    DWORD attrs = GetFileAttributesA(arg);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }
    return false;
}

int parse_command_line(int argc, char *argv[], search_criteria_t *criteria, cli_options_t *options) {
    criteria_init(criteria);
    init_options(options);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            options->show_help = true;
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            options->show_version = true;
            return 0;
        }
    }

    // Collect positional arguments (non-options)
    char *positional[2] = {NULL, NULL};
    int positional_count = 0;
    int options_start = argc;  // Index where we start parsing options

    for (int i = 1; i < argc && positional_count < 2; i++) {
        if (is_option(argv[i])) {
            options_start = i;
            break;
        }
        positional[positional_count++] = argv[i];
        options_start = i + 1;
    }


    char *pattern = NULL;
    char *path = NULL;

    if (positional_count == 0) {
        // fq → list all files in current directory
        pattern = "";
        path = ".";
    } else if (positional_count == 1) {
        // fq <arg> → is it a path or pattern?
        if (is_path(positional[0])) {
            pattern = "";
            path = positional[0];
        } else {
            pattern = positional[0];
            path = ".";
        }
    } else {
        // fq <arg1> <arg2> → pattern path (like fd)
        pattern = positional[0];
        path = positional[1];
    }

    criteria->root_path = _strdup(path);
    criteria->search_term = _strdup(pattern);

    if (!criteria->root_path || !criteria->search_term) {
        criteria_cleanup(criteria);
        return -1;
    }

    for (int i = options_start; i < argc; i++) {
        if (strcmp(argv[i], "--case") == 0 || strcmp(argv[i], "-c") == 0) {
            criteria->case_sensitive = true;
        } else if (strcmp(argv[i], "--glob") == 0 || strcmp(argv[i], "-g") == 0) {
            criteria->use_glob = true;
        } else if (strcmp(argv[i], "--regex") == 0 || strcmp(argv[i], "-r") == 0) {
            criteria->use_regex = true;
        } else if (strcmp(argv[i], "--no-skip") == 0) {
            criteria->skip_common_dirs = false;
        } else if (strcmp(argv[i], "--follow-symlinks") == 0 || strcmp(argv[i], "-L") == 0) {
            criteria->follow_symlinks = true;
        } else if (strcmp(argv[i], "--include-hidden") == 0 || strcmp(argv[i], "-H") == 0) {
            criteria->include_hidden = true;
        } else if (strcmp(argv[i], "--folders") == 0 || strcmp(argv[i], "--dirs") == 0) {
            criteria->include_directories = true;
        } else if (strcmp(argv[i], "--folders-only") == 0 || strcmp(argv[i], "--dirs-only") == 0) {
            criteria->include_directories = true;
            criteria->include_files = false;
        } else if (strcmp(argv[i], "--files-only") == 0) {
            criteria->include_directories = false;
            criteria->include_files = true;
        } else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) {
            options->quiet = true;
        } else if (strcmp(argv[i], "--ext") == 0 || strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (!criteria_parse_extensions(criteria, argv[i])) {
                criteria_cleanup(criteria);
                return -1;
            }
        } else if (strcmp(argv[i], "--type") == 0 || strcmp(argv[i], "-t") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (_stricmp(argv[i], "text") != 0 && _stricmp(argv[i], "image") != 0 &&
                _stricmp(argv[i], "video") != 0 && _stricmp(argv[i], "audio") != 0 &&
                _stricmp(argv[i], "archive") != 0) {
                fprintf(stderr, "Error: Invalid file type '%s'. Valid types: text, image, video, audio, archive\n", argv[i]);
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->file_type_filter = _strdup(argv[i]);
            if (!criteria->file_type_filter) {
                criteria_cleanup(criteria);
                return -1;
            }
        } else if (strcmp(argv[i], "--min") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (parse_size_arg(argv[i], &criteria->min_size) != 0) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->has_min_size = true;
        } else if (strcmp(argv[i], "--max") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (parse_size_arg(argv[i], &criteria->max_size) != 0) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->has_max_size = true;
        } else if (strcmp(argv[i], "--size") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            uint64_t size;
            char operator;
            if (parse_size_with_operator(argv[i], &size, &operator) != 0) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (operator == '+') {
                criteria->min_size = size + 1;
                criteria->has_min_size = true;
            } else if (operator == '-') {
                criteria->max_size = size - 1;
                criteria->has_max_size = true;
            } else {
                criteria->exact_size = size;
                criteria->has_exact_size = true;
            }
        } else if (strcmp(argv[i], "--after") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (parse_date_arg(argv[i], &criteria->after_time) != 0) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->has_after_time = true;
        } else if (strcmp(argv[i], "--before") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (parse_date_arg(argv[i], &criteria->before_time) != 0) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->has_before_time = true;
        } else if (strcmp(argv[i], "--created-after") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (parse_date_arg(argv[i], &criteria->after_ctime) != 0) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->has_after_ctime = true;
        } else if (strcmp(argv[i], "--created-before") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (parse_date_arg(argv[i], &criteria->before_ctime) != 0) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->has_before_ctime = true;
        } else if (strcmp(argv[i], "--max-results") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->max_results = (size_t)strtoull(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--max-depth") == 0 || strcmp(argv[i], "-d") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->max_depth = (size_t)strtoull(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--threads") == 0 || strcmp(argv[i], "-j") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->max_threads = (size_t)strtoull(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--timeout") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            criteria->timeout_ms = (DWORD)strtoul(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--out") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            options->output_file = argv[i];
        } else if (strcmp(argv[i], "--json") == 0) {
            options->json_output = true;
        } else if (strcmp(argv[i], "--color") == 0) {
            if (++i >= argc) {
                criteria_cleanup(criteria);
                return -1;
            }
            if (_stricmp(argv[i], "auto") == 0) {
                options->color_mode = COLOR_AUTO;
            } else if (_stricmp(argv[i], "always") == 0) {
                options->color_mode = COLOR_ALWAYS;
            } else if (_stricmp(argv[i], "never") == 0) {
                options->color_mode = COLOR_NEVER;
            } else {
                fprintf(stderr, "Error: Invalid color mode '%s'. Use auto|always|never.\n", argv[i]);
                criteria_cleanup(criteria);
                return -1;
            }
        } else if (strcmp(argv[i], "--preview") == 0) {
            criteria->preview_mode = true;
            if (i + 1 < argc && isdigit(argv[i + 1][0])) {
                i++;
                int lines = atoi(argv[i]);
                if (lines > 0 && lines <= 1000) {
                    criteria->preview_lines = (size_t)lines;
                }
            }
        } else if (strcmp(argv[i], "--stats") == 0) {
            options->show_stats = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            criteria_cleanup(criteria);
            return -1;
        }
    }

    return 0;
}

int output_results(const search_result_t *results, size_t count, const cli_options_t *options, const search_criteria_t *criteria) {
    FILE *fp = stdout;

    if (options->output_file) {
        fp = fopen(options->output_file, "w");
        if (!fp) {
            fprintf(stderr, "Error: Cannot open output file '%s'\n", options->output_file);
            return -1;
        }
    }

    output_format_t format = options->json_output ? OUTPUT_FORMAT_JSON : OUTPUT_FORMAT_TEXT;
    int result;

    if (criteria && criteria->preview_mode) {
        result = output_search_results_with_preview(fp, results, count, criteria, format);
    } else {
        result = output_search_results(fp, results, count, format);
    }

    if (options->output_file) {
        fclose(fp);
    }

    return result;
}
