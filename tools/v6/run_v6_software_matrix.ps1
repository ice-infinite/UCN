param([string]$BuildRoot = "")

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
        [string[]]$ConfigureArgs
    )
    $BuildDir = Join-Path $BuildRoot $Name
    & cmake -S $RepoRoot -B $BuildDir -G Ninja @ConfigureArgs
    if ($LASTEXITCODE -ne 0) { throw "$Name configure failed" }
    & cmake --build $BuildDir -j 6
    if ($LASTEXITCODE -ne 0) { throw "$Name build failed" }
    & ctest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "$Name tests failed" }
}

$Common = @("-DUCN_BUILD_TESTS=ON")
Invoke-MatrixCase "gcc-full-debug" ($Common + @("-DCMAKE_BUILD_TYPE=Debug", "-DUCN_PROFILE=FULL"))
Invoke-MatrixCase "gcc-full-release" ($Common + @("-DCMAKE_BUILD_TYPE=Release", "-DUCN_PROFILE=FULL"))
Invoke-MatrixCase "gcc-lite-debug" ($Common + @("-DCMAKE_BUILD_TYPE=Debug", "-DUCN_PROFILE=LITE"))
Invoke-MatrixCase "gcc-nano-debug" ($Common + @("-DCMAKE_BUILD_TYPE=Debug", "-DUCN_PROFILE=NANO"))
Invoke-MatrixCase "gcc-nano-feature-off" ($Common + @(
    "-DCMAKE_BUILD_TYPE=MinSizeRel",
    "-DUCN_PROFILE=NANO",
    "-DUCN_FEATURE_REALTIME=OFF",
    "-DUCN_FEATURE_CLUSTER=OFF",
    "-DUCN_FEATURE_ADAPTER=OFF"
))
Invoke-MatrixCase "gcc-nano-realtime-only" ($Common + @(
    "-DCMAKE_BUILD_TYPE=MinSizeRel",
    "-DUCN_PROFILE=NANO",
    "-DUCN_FEATURE_REALTIME=ON",
    "-DUCN_FEATURE_CLUSTER=OFF",
    "-DUCN_FEATURE_ADAPTER=OFF"
))
Invoke-MatrixCase "gcc-nano-cluster-only" ($Common + @(
    "-DCMAKE_BUILD_TYPE=MinSizeRel",
    "-DUCN_PROFILE=NANO",
    "-DUCN_FEATURE_REALTIME=OFF",
    "-DUCN_FEATURE_CLUSTER=ON",
    "-DUCN_FEATURE_ADAPTER=OFF"
))
Invoke-MatrixCase "gcc-nano-adapter-only" ($Common + @(
    "-DCMAKE_BUILD_TYPE=MinSizeRel",
    "-DUCN_PROFILE=NANO",
    "-DUCN_FEATURE_REALTIME=OFF",
    "-DUCN_FEATURE_CLUSTER=OFF",
    "-DUCN_FEATURE_ADAPTER=ON"
))

Write-Host "V6 software matrix completed at $BuildRoot"
