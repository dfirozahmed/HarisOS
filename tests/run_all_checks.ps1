$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    Write-Host '== HarisOS integration checks ==' -ForegroundColor Cyan

    $Bash = Get-Command bash -ErrorAction SilentlyContinue
    if (-not $Bash) {
        throw 'Bash was not found. Install Git for Windows or use MSYS2 UCRT64, then add bash.exe to PATH.'
    }

    $GfxTest = Join-Path $Root 'tests/run_gfx_tests.sh'
    $FontTest = Join-Path $Root 'tests/run_font_tests.sh'
    $EditTest = Join-Path $Root 'tests/run_edit_tests.sh'
    $WmTest = Join-Path $Root 'tests/run_wm_tests.sh'
    $PowerTest = Join-Path $Root 'tests/run_power_tests.sh'

    Write-Host "`n[1/6] Graphics regression tests" -ForegroundColor Yellow
    & $Bash.Source $GfxTest
    if ($LASTEXITCODE -ne 0) { throw "Graphics regression tests failed (exit $LASTEXITCODE)." }

    Write-Host "`n[2/6] Font regression tests" -ForegroundColor Yellow
    & $Bash.Source $FontTest
    if ($LASTEXITCODE -ne 0) { throw "Font regression tests failed (exit $LASTEXITCODE)." }

    Write-Host "`n[3/6] Text input tests" -ForegroundColor Yellow
    & $Bash.Source $EditTest
    if ($LASTEXITCODE -ne 0) { throw "Text input tests failed (exit $LASTEXITCODE)." }

    Write-Host "`n[4/6] Window resize tests" -ForegroundColor Yellow
    & $Bash.Source $WmTest
    if ($LASTEXITCODE -ne 0) { throw "Window resize tests failed (exit $LASTEXITCODE)." }

    Write-Host "`n[5/6] Power tests" -ForegroundColor Yellow
    & $Bash.Source $PowerTest
    if ($LASTEXITCODE -ne 0) { throw "Power tests failed (exit $LASTEXITCODE)." }

    Write-Host "`n[6/6] UEFI build" -ForegroundColor Yellow
    $CMake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $CMake) { throw 'CMake was not found. Install CMake and add it to PATH.' }

    & $CMake.Source -S . -B build
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }

    & $CMake.Source --build build --clean-first --parallel
    if ($LASTEXITCODE -ne 0) { throw "UEFI build failed (exit $LASTEXITCODE)." }

    Write-Host "`nAll integration checks passed." -ForegroundColor Green
    Write-Host 'Note: successful build/tests do not replace visual testing in QEMU or on real hardware.'
}
finally {
    Pop-Location
}
