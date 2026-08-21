param(
    [Parameter(Mandatory=$true)][string]$Dxc,
    [string]$Rga,
    [string]$RgaTarget,
    [string]$VulkanSdk = $env:VULKAN_SDK,
    [string]$OutDir,
    [switch]$RgaLive
)

$ErrorActionPreference = "Stop"
$ProbeRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$StartedUtc = (Get-Date).ToUniversalTime()

function Resolve-Executable {
    param(
        [Parameter(Mandatory=$true)][string]$Value,
        [Parameter(Mandatory=$true)][string]$Label
    )

    if (Test-Path -LiteralPath $Value) {
        return (Resolve-Path -LiteralPath $Value).Path
    }

    $command = Get-Command $Value -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "$Label not found: $Value"
}

if (-not $VulkanSdk) {
    throw "VULKAN_SDK is not set. Install/activate a Vulkan SDK or pass -VulkanSdk."
}
$VulkanSdk = (Resolve-Path -LiteralPath $VulkanSdk).Path
$Dxc = Resolve-Executable $Dxc "DXC"
$Cmake = Resolve-Executable "cmake" "CMake"
$Ninja = Resolve-Executable "ninja" "Ninja"
$Python = Resolve-Executable "python" "Python"

if ($Rga) {
    $Rga = Resolve-Executable $Rga "RGA"
}
if (($RgaTarget -or $RgaLive) -and -not $Rga) {
    throw "-RgaTarget and -RgaLive require -Rga."
}

$SpirvDis = Join-Path $VulkanSdk "Bin/spirv-dis.exe"
$SpirvVal = Join-Path $VulkanSdk "Bin/spirv-val.exe"
foreach ($tool in @($SpirvDis, $SpirvVal)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required Vulkan SDK tool not found: $tool"
    }
}

if (-not $OutDir) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutDir = Join-Path "out" "amd-full-qualification-$stamp"
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
if (Test-Path -LiteralPath $OutDir) {
    if (Get-ChildItem -LiteralPath $OutDir -Force | Select-Object -First 1) {
        throw "Output directory is not empty: $OutDir. Use a fresh -OutDir so stale evidence cannot be mistaken for this run."
    }
} else {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

$BuildDir = Join-Path $OutDir "driver-probe-build"
Write-Host "[AMD SPIR-V] Building installed-driver probe tools"
& $Cmake -G Ninja -S $ProbeRoot -B $BuildDir -DCMAKE_BUILD_TYPE=Release "-DVulkan_ROOT=$VulkanSdk"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $Cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$DriverProbe = Join-Path $BuildDir "amd_vulkan_isa_probe.exe"
$MatrixProbe = Join-Path $BuildDir "amd_vulkan_matrix_probe.exe"
$MixedDotProbe = Join-Path $BuildDir "amd_vulkan_mixed_dot_probe.exe"
$MathVerify = Join-Path $BuildDir "amd_vulkan_math_verify.exe"
$CandidateVerify = Join-Path $BuildDir "amd_vulkan_candidate_verify.exe"
foreach ($tool in @($DriverProbe, $MatrixProbe, $MixedDotProbe, $MathVerify, $CandidateVerify)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Expected probe executable was not built: $tool"
    }
}

$script:Results = @()
function Invoke-ProbeStep {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][ValidateSet("contract", "qualification")][string]$Category,
        [Parameter(Mandatory=$true)][scriptblock]$Action
    )

    Write-Host ""
    Write-Host "[AMD SPIR-V] $Name"
    $started = Get-Date
    $global:LASTEXITCODE = 0
    $exception = $null
    try {
        & $Action
        $rc = $global:LASTEXITCODE
        if ($null -eq $rc) { $rc = 0 }
    } catch {
        $rc = 1
        $exception = $_.Exception.Message
        Write-Host $exception -ForegroundColor Red
    }

    if ($rc -eq 0) {
        $status = "ok"
    } elseif ($Category -eq "contract") {
        $status = "failed"
    } else {
        $status = "incomplete"
    }

    $script:Results += [PSCustomObject]@{
        name = $Name
        category = $Category
        status = $status
        exit_code = [int]$rc
        duration_seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 3)
        exception = $exception
    }

    Write-Host ("[AMD SPIR-V] {0}: {1} (exit {2})" -f $Name, $status.ToUpperInvariant(), $rc)
}

Invoke-ProbeStep "SAD/QSad semantic proof" "contract" {
    & $Python (Join-Path $ProbeRoot "sad_semantics.py")
}

Invoke-ProbeStep "Raw-buffer probe self-test" "contract" {
    & $Python (Join-Path $ProbeRoot "raw_buffer_vector_probe.py") --self-test
}

$RawBufferArgs = @(
    (Join-Path $ProbeRoot "raw_buffer_vector_probe.py"),
    "--dxc", $Dxc,
    "--spirv-dis", $SpirvDis,
    "--spirv-val", $SpirvVal,
    "--target-env", "vulkan1.2",
    "--shader-profile", "cs_6_6",
    "--out-dir", (Join-Path $OutDir "raw-buffer"),
    "--require-untyped-vectorization",
    "--require-untyped-contract",
    "--require-validation"
)
Invoke-ProbeStep "Native untyped raw-buffer compiler contract" "contract" {
    & $Python @RawBufferArgs
}

Invoke-ProbeStep "Runtime math semantic verification" "contract" {
    $SemanticDir = Join-Path $OutDir "semantic"
    New-Item -ItemType Directory -Force -Path $SemanticDir | Out-Null
    $SemanticSpv = Join-Path $SemanticDir "math_execute.spv"
    $SemanticShader = Join-Path $ProbeRoot "shaders/math_execute.hlsl"
    & $Dxc -spirv -T cs_6_2 -E main -fspv-target-env=vulkan1.2 -fspv-extension=AMD `
        $SemanticShader -Fo $SemanticSpv
    if ($LASTEXITCODE -ne 0) { return }
    & $MathVerify $SemanticSpv
}

$QsadArgs = @(
    (Join-Path $ProbeRoot "apusr_qsad_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--spirv-dis", $SpirvDis,
    "--target-env", "vulkan1.2",
    "--shader-profile", "cs_6_6",
    "--out-dir", (Join-Path $OutDir "apusr-qsad")
)
if ($Rga) { $QsadArgs += @("--rga", $Rga) }
if ($RgaTarget) { $QsadArgs += @("--rga-target", $RgaTarget) }
if ($RgaLive) { $QsadArgs += "--rga-live" }
Invoke-ProbeStep "APUSR QSad/MSAD decisive installed-driver comparison" "qualification" {
    & $Python @QsadArgs
}

$SadArgs = @(
    (Join-Path $ProbeRoot "native_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--spirv-dis", $SpirvDis,
    "--out-dir", (Join-Path $OutDir "sad-msad")
)
if ($Rga) { $SadArgs += @("--rga", $Rga) }
if ($RgaTarget) { $SadArgs += @("--rga-target", $RgaTarget) }
if ($RgaLive) { $SadArgs += "--rga-live" }
Invoke-ProbeStep "General SAD/MSAD native recovery" "qualification" {
    & $Python @SadArgs
}

$DotArgs = @(
    (Join-Path $ProbeRoot "dot_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "dot")
)
Invoke-ProbeStep "Packed integer + packed-half FP16 dot native recovery" "qualification" {
    & $Python @DotArgs
}

$DotSatArgs = @(
    (Join-Path $ProbeRoot "dot_saturation_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "dot-saturation")
)
Invoke-ProbeStep "Saturating dot direct/canonical A/B" "qualification" {
    & $Python @DotSatArgs
}

$DotF16Args = @(
    (Join-Path $ProbeRoot "dot_f16_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--target-env", "vulkan1.2",
    "--out-dir", (Join-Path $OutDir "dot-f16")
)
Invoke-ProbeStep "FP16 dot canonical-recovery control" "qualification" {
    & $Python @DotF16Args
}

$MixedDotArgs = @(
    (Join-Path $ProbeRoot "mixed_dot_probe.py"),
    "--dxc", $Dxc,
    "--mixed-dot-probe", $MixedDotProbe,
    "--out-dir", (Join-Path $OutDir "mixed-dot")
)
Invoke-ProbeStep "Exact FP16/BF16 mixed-dot qualification" "qualification" {
    & $Python @MixedDotArgs
}

$BF16ConversionArgs = @(
    (Join-Path $ProbeRoot "bfloat16_conversion_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "bfloat16-conversion")
)
Invoke-ProbeStep "First-class BF16 conversion qualification" "qualification" {
    & $Python @BF16ConversionArgs
}

$CrosslaneArgs = @(
    (Join-Path $ProbeRoot "crosslane_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "crosslane")
)
Invoke-ProbeStep "APUSR cross-lane native recovery" "qualification" {
    & $Python @CrosslaneArgs
}

$ReductionArgs = @(
    (Join-Path $ProbeRoot "reduction_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "reduction")
)
Invoke-ProbeStep "Wave reduction direct/butterfly A/B" "qualification" {
    & $Python @ReductionArgs
}

$ConversionArgs = @(
    (Join-Path $ProbeRoot "conversion_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "half-conversion")
)
Invoke-ProbeStep "Half conversion scalar/packed A/B" "qualification" {
    & $Python @ConversionArgs
}

$PackingArgs = @(
    (Join-Path $ProbeRoot "packing_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "packing")
)
Invoke-ProbeStep "Packed normalized/integer conversion recovery" "qualification" {
    & $Python @PackingArgs
}

$MatrixArgs = @(
    (Join-Path $ProbeRoot "matrix_probe.py"),
    "--dxc", $Dxc,
    "--matrix-probe", $MatrixProbe,
    "--out-dir", (Join-Path $OutDir "matrix")
)
Invoke-ProbeStep "Cooperative matrix property + ISA qualification" "qualification" {
    & $Python @MatrixArgs
}

$TranscendentalArgs = @(
    (Join-Path $ProbeRoot "transcendental_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "transcendental")
)
Invoke-ProbeStep "Transcendental spelling A/B" "qualification" {
    & $Python @TranscendentalArgs
}

$LdsArgs = @(
    (Join-Path $ProbeRoot "lds_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "lds")
)
Invoke-ProbeStep "LDS scalar/vector access A/B" "qualification" {
    & $Python @LdsArgs
}

$CubeArgs = @(
    (Join-Path $ProbeRoot "cube_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "cube")
)
Invoke-ProbeStep "Cube helper native recovery" "qualification" {
    & $Python @CubeArgs
}

$ZooArgs = @(
    (Join-Path $ProbeRoot "candidate_zoo.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--runtime-verifier", $CandidateVerify,
    "--out-dir", (Join-Path $OutDir "candidate-zoo")
)
Invoke-ProbeStep "Empirical candidate zoo + runtime oracle" "qualification" {
    & $Python @ZooArgs
}

$MathArgs = @(
    (Join-Path $ProbeRoot "math_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "math")
)
Invoke-ProbeStep "Core math native recovery" "qualification" {
    & $Python @MathArgs
}

$ContractFailures = @($script:Results | Where-Object { $_.category -eq "contract" -and $_.exit_code -ne 0 })
$QualificationIncomplete = @($script:Results | Where-Object { $_.category -eq "qualification" -and $_.exit_code -ne 0 })
if ($ContractFailures.Count -gt 0) {
    $OverallStatus = "failed-contract"
    $FinalExitCode = 1
} elseif ($QualificationIncomplete.Count -gt 0) {
    $OverallStatus = "incomplete-qualification"
    $FinalExitCode = 2
} else {
    $OverallStatus = "complete"
    $FinalExitCode = 0
}

$FinishedUtc = (Get-Date).ToUniversalTime()
$Summary = [PSCustomObject]@{
    schema = 1
    overall_status = $OverallStatus
    started_utc = $StartedUtc.ToString("o")
    finished_utc = $FinishedUtc.ToString("o")
    duration_seconds = [math]::Round(($FinishedUtc - $StartedUtc).TotalSeconds, 3)
    dxc = $Dxc
    vulkan_sdk = $VulkanSdk
    spirv_dis = $SpirvDis
    spirv_val = $SpirvVal
    rga = $Rga
    rga_target = $RgaTarget
    rga_live = [bool]$RgaLive
    contract_failures = $ContractFailures.Count
    qualification_incomplete = $QualificationIncomplete.Count
    steps = @($script:Results)
}

$SummaryJson = Join-Path $OutDir "qualification-summary.json"
$SummaryText = Join-Path $OutDir "qualification-summary.txt"
$Summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $SummaryJson -Encoding utf8

$lines = @(
    "AMD SPIR-V full qualification",
    "overall_status: $OverallStatus",
    "dxc: $Dxc",
    "vulkan_sdk: $VulkanSdk",
    "contract_failures: $($ContractFailures.Count)",
    "qualification_incomplete: $($QualificationIncomplete.Count)",
    "",
    "steps:"
)
foreach ($result in $script:Results) {
    $lines += ("  [{0}] {1} - {2} (exit {3}, {4}s)" -f $result.category, $result.name, $result.status, $result.exit_code, $result.duration_seconds)
    if ($result.exception) {
        $lines += "    exception: $($result.exception)"
    }
}
$lines | Set-Content -LiteralPath $SummaryText -Encoding utf8

Write-Host ""
Write-Host "================ AMD SPIR-V qualification summary ================"
$script:Results | Format-Table category, status, exit_code, name -AutoSize
Write-Host "Overall: $OverallStatus"
Write-Host "Summary: $SummaryText"

$ArchivePath = "$OutDir-results.zip"
try {
    $ArchiveInputs = @(
        Get-ChildItem -LiteralPath $OutDir -Force |
            Where-Object { $_.Name -ne "driver-probe-build" } |
            Select-Object -ExpandProperty FullName
    )
    if ($ArchiveInputs.Count -gt 0) {
        Compress-Archive -Path $ArchiveInputs -DestinationPath $ArchivePath -Force
        Write-Host "Upload this result archive for analysis: $ArchivePath"
    }
} catch {
    Write-Warning "Could not create result archive: $($_.Exception.Message)"
    Write-Warning "The uncompressed results remain at: $OutDir"
}

exit $FinalExitCode
