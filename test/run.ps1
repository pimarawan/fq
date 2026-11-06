# (saved as UTF-8 with a BOM, or Windows PowerShell reads it as ANSI)
# End-to-end tests: builds a small tree on disk and checks what fq prints.
#   powershell -File test\run.ps1 build\fq.exe
param([string]$fq = "build\fq.exe")

$ErrorActionPreference = "Continue"
$fq = (Resolve-Path $fq).Path
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$OutputEncoding = [Text.Encoding]::UTF8

$root = Join-Path ([IO.Path]::GetTempPath()) ("fq-test-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory $root | Out-Null

function File($rel, $content = "x") {
    $p = Join-Path $root $rel
    New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
    [IO.File]::WriteAllText($p, $content)
}

File "a.txt" "hello`nworld`n"
File "b.TXT"
File "photo.jpg"
File "photo.PNG"
File ".dotfile.txt"
File "attr-hidden.txt"
(Get-Item -Force (Join-Path $root "attr-hidden.txt")).Attributes = "Hidden"
File "123"
File "abc"
File "report 2024.pdf" ("p" * 3000)
[IO.File]::WriteAllBytes((Join-Path $root "big.bin"), (New-Object byte[] (2MB)))
File "café.md"
File "日本語.txt"
File "src\main.c"
File "src\util.h"
File "build\out.o"
File "node_modules\pkg\index.js"
File "deep\1\2\3\4\f.txt"
File "Windows\w.txt"   # only C:\Windows is a system folder, not this one
cmd /c mklink /J "$root\link" "$root\src" | Out-Null
cmd /c mklink /J "$root\src\up" "$root" | Out-Null   # a loop, for -L

# a path past MAX_PATH (260)
$long = "\\?\" + $root + "\" + (("d" * 60 + "\") * 5) + "far.txt"
[IO.Directory]::CreateDirectory((Split-Path $long)) | Out-Null
[IO.File]::WriteAllText($long, "x")

$script:failed = 0
$script:run = 0

function Run([string[]]$argv) {
    $out = & $fq @argv 2>&1
    $script:code = $LASTEXITCODE
    @($out | ForEach-Object { "$_".Replace("$root\", "").Replace($root, ".") } | Sort-Object)
}

function Expect($name, [string[]]$argv, [string[]]$want) {
    $script:run++
    $got = Run $argv
    $w = @($want | Sort-Object)
    if (($got -join "|") -ne ($w -join "|") -or $script:code -ne 0) {
        $script:failed++
        Write-Host "FAIL $name"
        Write-Host "  fq $($argv -join ' ')  (exit $script:code)"
        Write-Host "  got:  $($got -join ', ')"
        Write-Host "  want: $($w -join ', ')"
    }
}

function ExpectTrue($name, $cond) {
    $script:run++
    if (-not $cond) { $script:failed++; Write-Host "FAIL $name" }
}

$farRel = (("d" * 60 + "\") * 5) + "far.txt"
$all = @("a.txt", "b.TXT", "photo.jpg", "photo.PNG", "123", "abc", "report 2024.pdf", "big.bin",
         "café.md", "日本語.txt", "src\main.c", "src\util.h", "deep\1\2\3\4\f.txt", "Windows\w.txt", $farRel)

try {
    Expect "everything, minus hidden, skipped folders and the junction" @("", $root, "--color", "never") $all
    Expect "trailing separator is not doubled" @("main", "$root\", "--color", "never") @("src\main.c")
    Expect "options before the pattern" @("--glob", "*.c", $root) @("src\main.c")
    Expect "a skipped folder is still listed when it matches" @("build", $root, "--folders-only") @("build")
    Expect "--no-skip goes in" @("", $root, "--no-skip", "--ext", "o,js") @("build\out.o", "node_modules\pkg\index.js")
    Expect "regex \D under ignore-case" @("^\D+$", $root, "--regex") @($all | Where-Object { (Split-Path $_ -Leaf) -notmatch '\d' })
    Expect "regex [^...]" @("^[^a-z]+$", $root, "--regex", "--case") @("123")
    Expect "glob braces, case ignored" @("*.{jpg,png}", $root, "--glob") @("photo.jpg", "photo.PNG")
    Expect "glob with --case" @("*.png", $root, "--glob", "--case") @()
    Expect "hidden by dot and by attribute" @("txt", $root, "-H", "--max-depth", "0") @("a.txt", "b.TXT", ".dotfile.txt", "attr-hidden.txt", "日本語.txt")
    Expect "-L follows both junctions, stops at the loop" @(".c", $root, "-L", "--ext", "c") @("src\main.c", "link\main.c")
    Expect "unicode substring" @("CAFÉ", $root) @("café.md")
    Expect "unicode, case-sensitive" @("CAFÉ", $root, "--case") @()
    Expect "max depth 0" @("", $root, "-d", "0", "--ext", "txt") @("a.txt", "b.TXT", "日本語.txt")
    Expect "max depth 1" @("", $root, "-d", "1", "--ext", "c,h,txt") @("a.txt", "b.TXT", "日本語.txt", "src\main.c", "src\util.h", "Windows\w.txt")
    Expect "--min" @("", $root, "--min", "1M") @("big.bin")
    Expect "--size + and --max" @("", $root, "--size", "+2000", "--max", "4K") @("report 2024.pdf")
    Expect "--ext with dots and spaces" @("", $root, "--ext", ".pdf, MD") @("report 2024.pdf", "café.md")
    Expect "--type image" @("", $root, "--type", "image") @("photo.jpg", "photo.PNG")
    Expect "--after the past" @("main", $root, "--after", "2000-01-01") @("src\main.c")
    Expect "--after the future" @("", $root, "--after", "2099-01-01") @()
    Expect "--created-before the past" @("", $root, "--created-before", "2000-01-01") @()
    Expect "folders" @("s", $root, "--folders", "-d", "0") @("src", "Windows", "node_modules")
    Expect "long paths" @("far", $root) @($farRel)
    Expect "one thread" @("", $root, "-j", "1") $all
    Expect "many threads" @("", $root, "-j", "32") $all

    $env:FQ_ENUM = "find"
    Expect "FindFirstFileEx path agrees" @("", $root) $all
    Expect "FindFirstFileEx sees reparse tags" @(".c", $root, "--ext", "c") @("src\main.c")
    Remove-Item Env:\FQ_ENUM

    $lines = Run @("", $root, "--max-results", "3")
    ExpectTrue "--max-results 3 prints 3" ($lines.Count -eq 3)

    $outFile = Join-Path $root "..\fq-out-$([Guid]::NewGuid().ToString('N')).txt"
    $printed = Run @("café", $root, "--out", $outFile)
    $written = [IO.File]::ReadAllText($outFile, [Text.Encoding]::UTF8).Trim()
    ExpectTrue "--out writes the file, prints nothing" ($printed.Count -eq 0 -and $written -eq "$root\café.md")
    Remove-Item $outFile

    $json = & $fq "report" $root --json | Out-String | ConvertFrom-Json
    ExpectTrue "--json is valid, with real backslashes" ($json.count -eq 1 -and $json.results[0].path -eq "$root\report 2024.pdf" -and $json.results[0].size -eq 3000)
    $none = & $fq "nothing-matches-this" $root --json | Out-String | ConvertFrom-Json
    ExpectTrue "--json with no results" ($none.count -eq 0 -and $none.results.Count -eq 0)

    $prev = & $fq "a.txt" $root --preview 1 -d 0
    ExpectTrue "--preview shows the first line" (($prev -join "|") -like "*a.txt|  hello|  ``[...``]*")

    & $fq "x" "$root\no-such-folder" 2>$null; ExpectTrue "missing folder exits 1" ($LASTEXITCODE -eq 1)
    & $fq "x" "$root\a.txt" 2>$null;          ExpectTrue "a file as root exits 1" ($LASTEXITCODE -eq 1)
    & $fq --frobnicate 2>$null;               ExpectTrue "unknown option exits 1" ($LASTEXITCODE -eq 1)
    & $fq "(" $root --regex 2>$null;          ExpectTrue "bad regex exits 1" ($LASTEXITCODE -eq 1)
    & $fq "" $root --min 5M --max 1M 2>$null; ExpectTrue "empty size range exits 1" ($LASTEXITCODE -eq 1)
    $help = & $fq --help | Out-String
    ExpectTrue "--help lists the created-* filters" ($help -match "created-after" -and $help -match "created-before")
}
catch {
    # anything that throws (bad JSON, say) is a failure, not an early exit
    $script:run++
    $script:failed++
    Write-Host "FAIL the test script stopped: $_"
}
finally {
    cmd /c rmdir "$root\link" | Out-Null
    cmd /c rmdir "$root\src\up" | Out-Null
    Remove-Item -Recurse -Force "\\?\$root"
}

Write-Host "$($script:run - $script:failed)/$($script:run) end-to-end tests passed"
exit [int]($script:failed -ne 0)
