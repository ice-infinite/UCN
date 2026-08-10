param(
    [string]$BuildDir = "build_scale_release",
    [string]$OutputDir = "docs\results\S21",
    [ValidateSet("local", "two-hop", "pairs", "incast", "all-to-all", "mixed")]
    [string]$Traffic = "two-hop",
    [ValidateSet("tree", "ring4")]
    [string]$Topology = "tree",
    [int[]]$Nodes = @(8, 16, 32, 64, 128, 256, 512, 1024),
    [int]$Ticks = 200,
    [int]$WarmupTicks = 150,
    [int]$WarmupBatch = 0,
    [int]$DrainTicks = 300,
    [int]$MessagesPerNode = 1,
    [int]$PayloadBytes = 16,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ResolvedBuildDir = Join-Path $RepoRoot $BuildDir
$ResolvedOutputDir = Join-Path $RepoRoot $OutputDir
$Executable = Join-Path $ResolvedBuildDir "ucn_scale_sim.exe"

if (-not $SkipBuild) {
    cmake -S $RepoRoot -B $ResolvedBuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    cmake --build $ResolvedBuildDir --target ucn_scale_sim --parallel
    if ($LASTEXITCODE -ne 0) { throw "Scale simulator build failed" }
}

if (-not (Test-Path -LiteralPath $Executable)) {
    throw "Scale simulator not found: $Executable"
}

New-Item -ItemType Directory -Force -Path $ResolvedOutputDir | Out-Null
$SummaryRows = @()

foreach ($NodeCount in $Nodes) {
    $PrefixName = "full_{0}_{1}_{2}" -f $NodeCount, $Topology, $Traffic
    $Prefix = Join-Path $ResolvedOutputDir $PrefixName
    & $Executable `
        --nodes $NodeCount `
        --ticks $Ticks `
        --warmup-ticks $WarmupTicks `
        --warmup-batch $WarmupBatch `
        --drain-ticks $DrainTicks `
        --topology $Topology `
        --traffic $Traffic `
        --messages-per-node $MessagesPerNode `
        --payload-bytes $PayloadBytes `
        --report-prefix $Prefix
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Scale case failed: nodes=$NodeCount traffic=$Traffic"
    }
    $SummaryPath = "${Prefix}_summary.csv"
    if (Test-Path -LiteralPath $SummaryPath) {
        $SummaryRows += Import-Csv -LiteralPath $SummaryPath
    }
}

$CombinedPath = Join-Path $ResolvedOutputDir (
    "ladder_{0}_{1}_summary.csv" -f $Topology, $Traffic)
$SummaryRows | Export-Csv -LiteralPath $CombinedPath -NoTypeInformation -Encoding utf8
Write-Host "UCN scale ladder summary: $CombinedPath"
