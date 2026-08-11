param(
    [string]$BuildDir = "build_scale_release",
    [string]$OutputDir = "docs\results\S21",
    [ValidateSet("local", "two-hop", "pairs", "incast", "all-to-all", "mixed")]
    [string]$Traffic = "two-hop",
    [ValidateSet("tree", "ring4")]
    [string]$Topology = "tree",
    [ValidateSet("FULL", "LITE")]
    [string]$BuildProfile = "FULL",
    [ValidateSet("fixed", "auto")]
    [string]$WireMode = "fixed",
    [ValidateSet("w0", "w1", "w2", "w3", "mixed")]
    [string[]]$WireProfiles = @("w3"),
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
    cmake -S $RepoRoot -B $ResolvedBuildDir -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DUCN_PROFILE=$BuildProfile `
        -DUCN_FEATURE_SERVICE=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    cmake --build $ResolvedBuildDir --target ucn_scale_sim --parallel
    if ($LASTEXITCODE -ne 0) { throw "Scale simulator build failed" }
}

if (-not (Test-Path -LiteralPath $Executable)) {
    throw "Scale simulator not found: $Executable"
}

New-Item -ItemType Directory -Force -Path $ResolvedOutputDir | Out-Null
$SummaryRows = @()
$ProfileRows = @()

foreach ($WireProfile in $WireProfiles) {
    foreach ($NodeCount in $Nodes) {
        if ($WireProfile -in @("w0", "mixed") -and $NodeCount -gt 254) {
            Write-Warning "Skip layout containing W0 above one-domain limit: wire=$WireProfile nodes=$NodeCount max=254"
            continue
        }
        $PrefixName = "{0}_{1}_{2}_{3}_{4}" -f `
            $BuildProfile.ToLowerInvariant(), $WireProfile, $NodeCount, `
            $Topology, $Traffic
        $Prefix = Join-Path $ResolvedOutputDir $PrefixName
        & $Executable `
            --nodes $NodeCount `
            --ticks $Ticks `
            --warmup-ticks $WarmupTicks `
            --warmup-batch $WarmupBatch `
            --drain-ticks $DrainTicks `
            --topology $Topology `
            --traffic $Traffic `
            --wire-mode $WireMode `
            --wire-profile $WireProfile `
            --messages-per-node $MessagesPerNode `
            --payload-bytes $PayloadBytes `
            --report-prefix $Prefix
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Scale case failed: wire=$WireProfile nodes=$NodeCount traffic=$Traffic"
        }
        $SummaryPath = "${Prefix}_summary.csv"
        if (Test-Path -LiteralPath $SummaryPath) {
            $SummaryRows += Import-Csv -LiteralPath $SummaryPath
        }
        $ProfilePath = "${Prefix}_profiles.csv"
        if (Test-Path -LiteralPath $ProfilePath) {
            $ProfileRows += Import-Csv -LiteralPath $ProfilePath
        }
    }
}

$CombinedPath = Join-Path $ResolvedOutputDir (
    "ladder_{0}_{1}_{2}_{3}_summary.csv" -f `
        $BuildProfile.ToLowerInvariant(), $WireMode, $Topology, $Traffic)
$SummaryRows | Export-Csv -LiteralPath $CombinedPath -NoTypeInformation -Encoding utf8
$CombinedProfilePath = Join-Path $ResolvedOutputDir (
    "ladder_{0}_{1}_{2}_{3}_profiles.csv" -f `
        $BuildProfile.ToLowerInvariant(), $WireMode, $Topology, $Traffic)
$ProfileRows | Export-Csv -LiteralPath $CombinedProfilePath -NoTypeInformation -Encoding utf8
Write-Host "UCN scale ladder summary: $CombinedPath"
Write-Host "UCN scale ladder profiles: $CombinedProfilePath"
