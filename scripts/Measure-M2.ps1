param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [ValidateRange(1, 1000)][int]$Warmup = 10,
    [ValidateRange(2, 10000)][int]$Samples = 50,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$viewerPath = Join-Path $projectRoot "build/$Configuration/AdaptiveClusterRendering.exe"
if (-not (Test-Path -LiteralPath $viewerPath -PathType Leaf)) {
    throw "Build $Configuration first: cmake --build --preset $($Configuration.ToLowerInvariant()) --parallel 1"
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ("build/benchmarks/m2-" + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
}
$reportDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$modelPath = Join-Path $projectRoot 'assets/suzanne.obj'
$cases = @(
    @{ Name = 'suzanne-source'; Args = @($modelPath) },
    @{ Name = 'suzanne-n1'; Args = @($modelPath, '--subdivision', '1') },
    @{ Name = 'suzanne-n2-waves'; Args = @($modelPath, '--subdivision', '2', '--displacement', '0.25') },
    @{ Name = 'suzanne-n4-waves'; Args = @($modelPath, '--subdivision', '4', '--displacement', '0.25') },
    @{ Name = 'suzanne-n8-waves'; Args = @($modelPath, '--subdivision', '8', '--displacement', '0.25') },
    @{ Name = 'suzanne-n8-off'; Args = @($modelPath, '--subdivision', '8') },
    @{ Name = 'grid-n8-waves'; Args = @('--fixture', 'grid', '--subdivision', '8', '--displacement', '0.25') }
)
$summaryRows = @()
foreach ($case in $cases) {
    $reportPath = Join-Path $reportDirectory ($case.Name + '.json')
    $viewerArgs = $case.Args + @('--benchmark', $reportPath, '--benchmark-warmup', "$Warmup", '--benchmark-samples', "$Samples")
    # Sequential runs: GPU benchmarks must not compete with one another.
    & $viewerPath @viewerArgs
    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $($case.Name)" }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.labels.build_config -ne $Configuration -or $report.measured_runs -ne $Samples) {
        throw "Unexpected report configuration: $($case.Name)"
    }
    $compute = $report.summary_ms.gpu_generate
    $render = $report.summary_ms.gpu_render
    $cpu = $report.summary_ms.cpu_generate_and_wait
    $summaryRows += [pscustomobject]@{
        Case = $case.Name; Triangles = $report.counts.drawn_triangles
        GPUGenerateMedianMs = $compute.median; GPUGenerateP95Ms = $compute.p95
        GPUGenerateMinMs = $compute.min; GPUGenerateMaxMs = $compute.max
        GPURenderMedianMs = $render.median; GPURenderP95Ms = $render.p95
        GPURenderMinMs = $render.min; GPURenderMaxMs = $render.max
        CPUGenerateWaitMedianMs = $cpu.median; CPUGenerateWaitP95Ms = $cpu.p95
        OutputAllocationBytes = $report.counts.output_allocation_bytes
        GPU = $report.labels.gpu; Build = $report.labels.build_config; Report = $reportPath
    }
}
$summaryRows | Export-Csv -LiteralPath (Join-Path $reportDirectory 'summary.csv') -NoTypeInformation -Encoding UTF8
$summaryRows | Format-Table Case, Triangles, GPUGenerateMedianMs, GPURenderMedianMs, CPUGenerateWaitMedianMs -AutoSize
Write-Host "Reports: $reportDirectory"
