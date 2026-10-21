$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    Write-Host '== HarisOS integration checks ==' -ForegroundColor Cyan

    $Bash = Get-Command bash -ErrorAction SilentlyContinue
    if (-not $Bash) {
        throw 'Bash was not found. Install Git for Windows or use MSYS2 UCRT64, then add bash.exe to PATH.'
    }

    # host-side tests: (title, script). The block / FAT / volume tests read build/haris.img if it exists, so
    # run a build first for the full check (the last step builds again, clean).
    $Tests = @(
        @('Graphics regression tests', 'tests/run_gfx_tests.sh'),
        @('Font regression tests', 'tests/run_font_tests.sh'),
        @('Text input tests', 'tests/run_edit_tests.sh'),
        @('Window manager tests', 'tests/run_wm_tests.sh'),
        @('Power tests', 'tests/run_power_tests.sh'),
        @('I2C-HID discovery tests', 'tests/run_i2chid_tests.sh'),
        @('PCI and Device Manager tests', 'tests/run_devmgr_tests.sh'),
        @('Virtual file system tests', 'tests/run_vfs_tests.sh'),
        @('Block device and partition tests', 'tests/run_blk_tests.sh'),
        @('USB mass storage tests', 'tests/run_usbmsc_tests.sh'),
        @('USB drive hot-plug tests', 'tests/run_usbvol_tests.sh'),
        @('FAT32 tests', 'tests/run_fat32_tests.sh'),
        @('exFAT tests', 'tests/run_exfat_tests.sh'),
        @('exFAT write tests', 'tests/run_exfatw_tests.sh'),
        @('NTFS tests', 'tests/run_ntfs_tests.sh'),
        @('NTFS write tests', 'tests/run_ntfsw_tests.sh'),
        @('Volume (drives in the VFS) tests', 'tests/run_volume_tests.sh')
    )
    $Total = $Tests.Count + 1

    $n = 0
    foreach ($t in $Tests) {
        $n++
        Write-Host "`n[$n/$Total] $($t[0])" -ForegroundColor Yellow
        & $Bash.Source (Join-Path $Root $t[1])
        if ($LASTEXITCODE -ne 0) { throw "$($t[0]) failed (exit $LASTEXITCODE)." }
    }

    Write-Host "`n[$Total/$Total] UEFI build" -ForegroundColor Yellow
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
