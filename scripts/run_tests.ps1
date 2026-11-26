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
Extract-Fn "static bool np_match(const char *p, const char *q, size_t n, bool match_case)" (Join-Path $out "np_match.impl")
Extract-Fn "static bool np_word_char(unsigned char c)" (Join-Path $out "np_word_char.impl")
Extract-Fn "static bool np_hex_parse_bytes(const char *s, bool ascii, unsigned char *out, int cap, int *out_n)" (Join-Path $out "np_hex_parse_bytes.impl")
Extract-Fn "static size_t np_row_tab2sp(const char *row, char *out, int tab)" (Join-Path $out "np_row_tab2sp.impl")
Extract-Fn "static size_t np_row_sp2tab(const char *row, char *out, int tab)" (Join-Path $out "np_row_sp2tab.impl")
Extract-Fn "static bool np_word_edge(const char *text, size_t len, size_t i, size_t n, bool ww)" (Join-Path $out "np_word_edge.impl")

# Static config-key consistency check (needs no compiler).
Write-Host "== check_config ==" -ForegroundColor Cyan
python (Join-Path $repo "scripts\check_config.py")
if ($LASTEXITCODE -ne 0) { Write-Host "CONFIG CHECK FAILED" -ForegroundColor Red; $fail++ }
else { Write-Host "PASSED" -ForegroundColor Green }

$tests = @(
    @{ name = "test_fence";   src = "scripts\test_fence.c";   link = @() },
    @{ name = "test_inline";  src = "scripts\test_inline.c";  link = @() },
    @{ name = "test_openai";  src = "scripts\test_openai.c";  link = @("src\np_openai.c") },
    @{ name = "test_maclaw";  src = "scripts\test_maclaw.c";  link = @("src\np_maclaw.c") },
    @{ name = "test_match";   src = "scripts\test_match.c";    link = @() },
    @{ name = "test_hexfind"; src = "scripts\test_hexfind.c";  link = @() },
    @{ name = "test_transform"; src = "scripts\test_transform.c"; link = @() },
    # shaper vs HarfBuzz reference (P4 #17); needs the repo root as cwd for assets/fonts
    @{ name = "test_lig"; src = "scripts\test_lig.c"; link = @("vendor\CUI\src\cui_shape.c");
       include = @("vendor\CUI\src"); runIn = "repo" },
    # S0 AI-native底座 (A1/A3/A2/B5/H1/H2/H8/X1/X2/T1/T2/A4) - each test links only its modules
    @{ name = "test_s0_job";       src = "scripts\test_s0_job.c";       link = @("src\np_job.c") },
    @{ name = "test_s0_patch";     src = "scripts\test_s0_patch.c";     link = @("src\np_patch.c") },
    @{ name = "test_s0_binary";    src = "scripts\test_s0_binary.c";    link = @("src\np_bytepatch.c","src\np_integrity.c","src\np_filesafety.c") },
    @{ name = "test_s0_capability";src = "scripts\test_s0_capability.c";link = @("src\np_capability.c") },
    @{ name = "test_s0_toolchain"; src = "scripts\test_s0_toolchain.c"; link = @("src\np_toolchain.c") },
    @{ name = "test_s0_context";   src = "scripts\test_s0_context.c";   link = @("src\np_context.c","src\np_citation.c","src\np_integrity.c","src\np_job.c") },
    @{ name = "test_s0_index";     src = "scripts\test_s0_index.c";     link = @("src\np_index.c","src\np_integrity.c") },
    @{ name = "test_s1_probe";     src = "scripts\test_s1_probe.c";     link = @("src\np_probe.c") },
    @{ name = "test_s1_toolinstall"; src = "scripts\test_s1_toolinstall.c"; link = @("src\np_toolinstall.c","src\np_toolchain.c","src\np_integrity.c") },
    @{ name = "test_s1_skillmgr";  src = "scripts\test_s1_skillmgr.c";  link = @("src\np_skillmgr.c","src\np_texttools.c","src\np_capability.c","src\np_patch.c","src\np_job.c","src\np_integrity.c") }
)

$fail = 0
foreach ($t in $tests) {
    Write-Host "== $($t.name) ==" -ForegroundColor Cyan
    # Pass extra include dirs through the INCLUDE env var: PS 5.1 mangles
    # quoting when an array of space-containing paths is expanded inline.
    if ($t.include) {
        $env:INCLUDE = "$env:INCLUDE;" + (($t.include | ForEach-Object { Join-Path $repo $_ }) -join ';')
    }
    $srcs = @((Join-Path $repo $t.src)) + ($t.link | ForEach-Object { Join-Path $repo $_ })
    cl /nologo /utf-8 /W4 /I (Join-Path $repo "src") /I $out /Fe:"$($t.name).exe" @srcs |
        Select-String -Pattern "error|warning"
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; $fail++; continue }
    $exe = ".\$($t.name).exe"
    if ($t.runIn -eq "repo") { Push-Location $repo; $exe = Join-Path $out ($t.name + ".exe") }
    & $exe
    if ($LASTEXITCODE -ne 0) { Write-Host "TEST FAILED" -ForegroundColor Red; $fail++ }
    else { Write-Host "PASSED" -ForegroundColor Green }
    if ($t.runIn -eq "repo") { Pop-Location }
}

Pop-Location
if ($fail -gt 0) { Write-Host "`n$fail test(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nALL TESTS PASSED" -ForegroundColor Green
