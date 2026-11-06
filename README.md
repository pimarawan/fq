# fq

Find files on Windows. Like fd, but written for NTFS, in C. One exe, no dependencies.

    fq [pattern] [path] [options]

```
fq report                       names containing "report"
fq "*.{jpg,png}" D:\ --glob     pictures on D:
fq "^\d{4}-" --regex            names starting with a year
fq "" C:\ --min 1G              files over 1 GB on C:
fq build --folders-only         folders named build
```

Case is ignored unless you pass `--case`. `fq --help` has the rest.

## Defaults

fq does not go into `node_modules .git .svn .hg __pycache__ .vs obj bin Debug
Release packages bower_components dist build`, or, at the top of a drive,
`Windows`, `Program Files`, `ProgramData`, `$RECYCLE.BIN` and the like.
A skipped folder is still listed if it matches. Hidden files (a leading dot or
the hidden attribute) are left out. `--no-skip` and `-H` turn both off.

## Speed

Warm cache, Ryzen 5 5600G (6 cores, 12 threads), Windows 11, minimum of 10
runs. Same work for every tool: all files, hidden included, nothing skipped or
ignored (`fq -H --no-skip`, `fd -u -t f`).

|         | C:\Program Files, SSD, 92k files | E:\work\Cake, HDD, 11k files |
| ------- | -------------------------------: | ---------------------------: |
| fq 0.9  |                           156 ms |                        33 ms |
| fd 10.5 |                           194 ms |                        49 ms |
| fq 0.8  |                    found nothing |                        87 ms |

Glob and regex searches cost the same as listing everything. `fq --version`
takes 12 ms to start, fd 20 ms. fd finds 28 fewer files in Program Files
because `-t f` leaves out symlinks.

    powershell -File test\bench.ps1 -root <folder> [-fd fd.exe] [-old old-fq.exe]

## How

Each folder is read with `GetFileInformationByHandleEx(FileFullDirectoryInfo)`,
one call per few hundred entries. `FindFirstFileExW` with `FindExInfoBasic` and
`FIND_FIRST_EX_LARGE_FETCH` takes 250 ms on the same tree as above. The buffer
size (4 to 256 KiB) makes no difference.

Threads share the work by stealing: a thread works through its own folders
newest first, and an idle one takes the oldest folder from another thread.
Default is one thread per CPU, at most 8:

| threads | 1   | 2   | 4   | 8   | 12  |
| ------- | --- | --- | --- | --- | --- |
| ms      | 743 | 398 | 219 | 154 | 185 |

Names stay UTF-16 from the kernel to the matcher. Only printed paths are
converted, and the console gets them through `WriteConsoleW`, so any name
prints right.

The regex engine is a Thompson NFA: linear time, no backtracking.
`^(a|aa)+$` on forty a's is instant.

Only kernel32 and the C runtime are linked.

## Build

    make          MinGW gcc, writes build/fq.exe
    build.bat     MSVC
    make test     or build.bat test

## 0.9

Rewritten. Fixed:

- `--regex` without `--case` lowercased the pattern, so `\D` became `\d`
- regex had no `|` or groups, `[^...]` was broken, and it was not thread-safe
- nothing was found inside `C:\Program Files` or `C:\Windows`, even with `--no-skip`
- `fq build --folders` found nothing
- `--out` without `--json` wrote nothing
- `--json` turned `\` into `/`
- paths over 520 bytes were dropped
- the hidden attribute was ignored
- `-L` had no cycle check
- searches stopped after 5 minutes by default

Also: options can go anywhere, `--opt=value` works, `--` ends options.

## License

MIT
