[CmdletBinding()]
param(
    [string]$FirmwareRoot = 'E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1',
    [switch]$SkipFirmware
)

$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Label,
        [Parameter(Mandatory = $true)]
        [string]$Command,
        [string[]]$Arguments = @()
    )

    Write-Host "==> $Label"
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cmakeCommand = (Get-Command cmake -ErrorAction Stop).Source
$profiles = @(
    [pscustomobject]@{ Name = 'Debug'; Directory = 'build'; BuildType = 'Debug'; CFlags = '' },
    [pscustomobject]@{ Name = 'Release'; Directory = 'build-release'; BuildType = 'Release'; CFlags = '' },
    [pscustomobject]@{ Name = 'MTU64'; Directory = 'build-mtu64'; BuildType = 'Debug'; CFlags = '-DUCN_MAX_FRAME_BYTES=64' },
    [pscustomobject]@{ Name = 'Bearer1'; Directory = 'build-bearer1'; BuildType = 'Debug'; CFlags = '-DUCN_MAX_BEARERS_PER_NEIGHBOR=1' }
)

Write-Host 'T22.7.1 automatic regression: no upload, monitor, reset, or serial-port access.'
foreach ($profile in $profiles) {
    $buildRoot = Join-Path $projectRoot $profile.Directory
    $configureArguments = @(
        '-S', $projectRoot,
        '-B', $buildRoot,
        '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$($profile.BuildType)",
        '-DUCN_BUILD_TESTS=ON'
    )
    if ($null -ne $profile.CFlags) {
        $configureArguments += "-DCMAKE_C_FLAGS=$($profile.CFlags)"
    }
    Invoke-Checked -Label "configure $($profile.Name)" -Command $cmakeCommand -Arguments $configureArguments
    Invoke-Checked -Label "build $($profile.Name)" -Command $cmakeCommand -Arguments @('--build', $buildRoot, '--parallel', '2')
    Invoke-Checked -Label "ctest $($profile.Name)" -Command $cmakeCommand -Arguments @('--build', $buildRoot, '--target', 'test')
}

Invoke-Checked -Label 'git diff check' -Command 'git' -Arguments @('-C', $projectRoot, 'diff', '--check')

if (-not $SkipFirmware) {
    if (-not (Test-Path -LiteralPath $FirmwareRoot -PathType Container)) {
        throw "FirmwareRoot does not exist: $FirmwareRoot"
    }
    $pioCommand = Get-Command platformio -ErrorAction SilentlyContinue
    if ($null -eq $pioCommand) {
        $defaultPio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'
        if (-not (Test-Path -LiteralPath $defaultPio -PathType Leaf)) {
            throw 'PlatformIO was not found. Re-run with -SkipFirmware or install PlatformIO.'
        }
        $pioPath = $defaultPio
    } else {
        $pioPath = $pioCommand.Source
    }
    Invoke-Checked -Label 'build ESP32-S3 Node A' -Command $pioPath -Arguments @(
        'run', '-d', $FirmwareRoot, '-e', 'esp32s3_120_16_8-qio_opi_node_a'
    )
    Invoke-Checked -Label 'build ESP32-S3 Node B' -Command $pioPath -Arguments @(
        'run', '-d', $FirmwareRoot, '-e', 'esp32s3_120_16_8-qio_opi_node_b'
    )
    Invoke-Checked -Label 'build ESP-WROOM-32' -Command $pioPath -Arguments @(
        'run', '-d', $FirmwareRoot, '-e', 'esp32_wroom_32'
    )
}

Write-Host 'T22.7.1 automatic regression passed.'
