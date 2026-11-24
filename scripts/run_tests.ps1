# One-shot regression runner for RapidEditor's standalone unit tests.
# Builds each test with MSVC (cl) and runs it, printing a summary.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\run_tests.ps1
#
# Each entry lists the test program and the implementation sources it links
# against (the tests stub out the network transport themselves).
$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$vsRoot = "C:\Program Files\Microsoft Visual Studio"
$vsDir = $null
foreach ($ed in Get-ChildItem $vsRoot -Directory -ErrorAction SilentlyContinue) {
    foreach ($c in Get-ChildItem $ed.FullName -Directory -ErrorAction SilentlyContinue) {
        $msvc = "$($c.FullName)\VC\Tools\MSVC"
        if (Test-Path $msvc) {
            $latest = Get-ChildItem $msvc -Directory | Sort-Object Name | Select-Object -Last 1
            if ($latest) { $vsDir = $latest.FullName }
        }
    }
}
if (-not $vsDir) { Write-Error "MSVC toolset not found under $vsRoot" }
$sdk = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer = Get-ChildItem "$sdk\Include" -Directory | Where-Object { $_.Name -like "10.*" } |
          Sort-Object Name | Select-Object -Last 1
if (-not $sdkVer) { Write-Error "Windows SDK not found under $sdk" }
$ver = $sdkVer.Name

$env:INCLUDE = "$vsDir\include;$sdk\Include\$ver\ucrt;$sdk\Include\$ver\shared;$sdk\Include\$ver\um"
$env:LIB = "$vsDir\lib\x64;$sdk\Lib\$ver\ucrt\x64;$sdk\Lib\$ver\um\x64"
$env:PATH = "$vsDir\bin\Hostx64\x64;$env:PATH"

$out = Join-Path $env:TEMP "rapideditor-tests"
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $out

# Extract functions verbatim from src/rapideditor.c so the standalone tests
# always exercise the real implementation (impl files land in $out, found
# via the /I include path). A function ends at the first column-0 '}'.
# NOTE: keep the assembly explicit (string + [char]10) - interpolating the
# regex group plus `n inside one double-quoted string corrupted the tail.
$srcRaw = Get-Content (Join-Path $repo "src\rapideditor.c") -Raw
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Extract-Fn([string]$signature, [string]$implPath) {
    $pat = [regex]::Escape($signature) + "\s*\{(.*?)\r?\n\}"
    $m = [regex]::Match($srcRaw, $pat, [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $m.Success) { throw "cannot extract function: $signature" }
    $impl = $signature + " {" + [char]10 + $m.Groups[1].Value + [char]10 + "}"
    [System.IO.File]::WriteAllText($implPath, $impl, $utf8NoBom)
}
Extract-Fn "static void np_ai_strip_code_fence(char *buf, size_t cap)" (Join-Path $out "np_ai_strip_code_fence.impl")
Extract-Fn "static char *np_inline_clean(char *buf)" (Join-Path $out "np_inline_clean.impl")

$tests = @(
    @{ name = "test_fence";   src = "scripts\test_fence.c";   link = @() },
    @{ name = "test_inline";  src = "scripts\test_inline.c";  link = @() },
    @{ name = "test_openai";  src = "scripts\test_openai.c";  link = @("src\np_openai.c") },
    @{ name = "test_maclaw";  src = "scripts\test_maclaw.c";  link = @("src\np_maclaw.c") }
)

$fail = 0
foreach ($t in $tests) {
    Write-Host "== $($t.name) ==" -ForegroundColor Cyan
    $srcs = @((Join-Path $repo $t.src)) + ($t.link | ForEach-Object { Join-Path $repo $_ })
    cl /nologo /utf-8 /W4 /I (Join-Path $repo "src") /I $out /Fe:"$($t.name).exe" @srcs |
        Select-String -Pattern "error|warning"
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; $fail++; continue }
    & ".\$($t.name).exe"
    if ($LASTEXITCODE -ne 0) { Write-Host "TEST FAILED" -ForegroundColor Red; $fail++ }
    else { Write-Host "PASSED" -ForegroundColor Green }
}

Pop-Location
if ($fail -gt 0) { Write-Host "`n$fail test(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nALL TESTS PASSED" -ForegroundColor Green
