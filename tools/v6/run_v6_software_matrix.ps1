param(
    [string]$BuildRoot = "",
    [switch]$IncludeLegacyRegression
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $RepoRoot ".v6-matrix"
}
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
if (-not $BuildRoot.StartsWith($RepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildRoot must remain inside the UCN workspace: $BuildRoot"
}
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

function Invoke-MatrixCase {
    param(
        [string]$Name,
        [string[]]$ConfigureArgs,
        [string]$TestRegex = "^(ucn_v6_|v6_)"
    )
    $BuildDir = Join-Path $BuildRoot $Name
    & cmake -S $RepoRoot -B $BuildDir -G Ninja @ConfigureArgs
    if ($LASTEXITCODE -ne 0) { throw "$Name configure failed" }
    & cmake --build $BuildDir -j 6
    if ($LASTEXITCODE -ne 0) { throw "$Name build failed" }
    if ($IncludeLegacyRegression) {
        & ctest --test-dir $BuildDir --output-on-failure
    } else {
        & ctest --test-dir $BuildDir -R $TestRegex --output-on-failure
    }
    if ($LASTEXITCODE -ne 0) { throw "$Name tests failed" }
}

$Common = @("-DUCN_BUILD_TESTS=ON", "-DUCN_BUILD_V6_EXPERIMENTAL=ON")
Invoke-MatrixCase "gcc-full-debug" ($Common + @("-DCMAKE_BUILD_TYPE=Debug", "-DUCN_PROFILE=FULL"))
Invoke-MatrixCase "gcc-full-release" ($Common + @("-DCMAKE_BUILD_TYPE=Release", "-DUCN_PROFILE=FULL"))
Invoke-MatrixCase "gcc-lite-debug" ($Common + @("-DCMAKE_BUILD_TYPE=Debug", "-DUCN_PROFILE=LITE"))
Invoke-MatrixCase "gcc-nano-debug" ($Common + @("-DCMAKE_BUILD_TYPE=Debug", "-DUCN_PROFILE=NANO"))
Invoke-MatrixCase "gcc-service-off" ($Common + @("-DCMAKE_BUILD_TYPE=Debug", "-DUCN_FEATURE_SERVICE=OFF"))
Invoke-MatrixCase "gcc-min-adapter" ($Common + @(
    "-DCMAKE_BUILD_TYPE=MinSizeRel",
    "-DCMAKE_C_FLAGS=-DUCN_V6_CONFIG_ADAPTER_LINKS=1 -DUCN_V6_CONFIG_ADAPTER_RX_SLOTS=1 -DUCN_V6_CONFIG_ADAPTER_TX_SLOTS=1 -DUCN_V6_CONFIG_ADAPTER_FRAME_BYTES=64"
)) "^(ucn_v6_config_contract_tests|v6_source_boundary_gate|v6_resource_report|v6_evidence_validator_selftest)$"

Write-Host "V6 software matrix completed at $BuildRoot"
