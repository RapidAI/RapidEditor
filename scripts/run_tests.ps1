# One-shot regression runner for RapidEditor's standalone unit tests.
# Builds each test with MSVC (cl) and runs it, printing a summary.
# Usage: powershell -File scripts\run_tests.ps1
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

$tests = @(
    @{ name = "test_fence";   src = "scripts\test_fence.c";   link = @() },
    @{ name = "test_openai";  src = "scripts\test_openai.c";  link = @("src\np_openai.c") },
    @{ name = "test_maclaw";  src = "scripts\test_maclaw.c";  link = @("src\np_maclaw.c") }
)

$fail = 0
foreach ($t in $tests) {
    Write-Host "== $($t.name) ==" -ForegroundColor Cyan
    $srcs = @((Join-Path $repo $t.src)) + ($t.link | ForEach-Object { Join-Path $repo $_ })
    cl /nologo /utf-8 /W4 /I (Join-Path $repo "src") /Fe:"$($t.name).exe" @srcs |
        Select-String -Pattern "error|warning"
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; $fail++; continue }
    & ".\$($t.name).exe"
    if ($LASTEXITCODE -ne 0) { Write-Host "TEST FAILED" -ForegroundColor Red; $fail++ }
    else { Write-Host "PASSED" -ForegroundColor Green }
}

Pop-Location
if ($fail -gt 0) { Write-Host "`n$fail test(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nALL TESTS PASSED" -ForegroundColor Green
