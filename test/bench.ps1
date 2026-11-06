# powershell -File test\bench.ps1 -root "C:\Program Files" [-old E:\fq\fq.exe] [-fd fd.exe]
# Warm cache. Prints min and median ms of 10 runs, and the result count.
param([string]$root, [string]$fq = "build\fq.exe", [string]$old = "", [string]$fd = "fd", [int]$runs = 10)

$bench = Join-Path $PSScriptRoot "..\build\bench.exe"
$fq = (Resolve-Path $fq).Path

function Row($name, [string[]]$cmd) {
    $n = (& $cmd[0] $cmd[1..($cmd.Count - 1)] 2>$null | Measure-Object -Line).Lines
    $t = (& $bench $runs 2 @cmd) -split " "
    "| {0,-34} | {1,8} | {2,8} | {3,7} |" -f $name, $t[0], $t[1], $n
}

"| {0,-34} | {1,8} | {2,8} | {3,7} |" -f "$root", "min ms", "median", "results"
"|{0}|{1}|{2}|{3}|" -f ("-" * 36), ("-" * 10), ("-" * 10), ("-" * 9)

# the same work: every file, hidden too, nothing skipped or ignored
Row "fq, everything"            @($fq, "", $root, "-H", "--no-skip")
if ($old) { Row "fq 0.8, everything"  @($old, "", $root, "-H", "--no-skip") }
if ($fd)  { Row "fd -u -t f, everything" @($fd, "-u", "-t", "f", ".", $root) }
Row "fq, glob *.json"           @($fq, "*.json", $root, "--glob", "-H", "--no-skip")
if ($old) { Row "fq 0.8, glob *.json" @($old, "*.json", $root, "--glob", "-H", "--no-skip") }
if ($fd)  { Row "fd, glob *.json"     @($fd, "-u", "-t", "f", "-g", "*.json", $root) }
Row "fq, regex \.(js|ts)$"      @($fq, "\.(js|ts)$", $root, "--regex", "-H", "--no-skip")
if ($old) { Row "fq 0.8, regex \.(js|ts)$" @($old, "\.(js|ts)$", $root, "--regex", "-H", "--no-skip") }
if ($fd)  { Row "fd, regex \.(js|ts)$" @($fd, "-u", "-t", "f", "\.(js|ts)$", $root) }

# fq's own choices
foreach ($j in 1, 2, 4, 8, 12) { Row "fq -j $j" @($fq, "", $root, "-H", "--no-skip", "-j", "$j") }
$env:FQ_ENUM = "find"; Row "fq, FindFirstFileExW" @($fq, "", $root, "-H", "--no-skip"); Remove-Item Env:\FQ_ENUM
foreach ($kb in 4, 16, 64, 256) { $env:FQ_BUF = "$kb"; Row "fq, $kb KiB buffer" @($fq, "", $root, "-H", "--no-skip"); Remove-Item Env:\FQ_BUF }
