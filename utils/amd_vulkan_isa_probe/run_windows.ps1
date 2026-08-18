param(
    [Parameter(Mandatory=$true)][string]$Dxc,
    [string]$Rga,
    [string]$RgaTarget,
    [string]$VulkanSdk = $env:VULKAN_SDK,
    [string]$OutDir = "out/amd-native-probe",
    [switch]$RgaLive
)

$ErrorActionPreference = "Stop"
$ProbeRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $OutDir "driver-probe-build"

if (-not $VulkanSdk) {
    throw "VULKAN_SDK is not set. Install/activate a Vulkan SDK or pass -VulkanSdk."
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
cmake -G Ninja -S $ProbeRoot -B $BuildDir -DCMAKE_BUILD_TYPE=Release -DVulkan_ROOT="$VulkanSdk"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$DriverProbe = Join-Path $BuildDir "amd_vulkan_isa_probe.exe"
$MatrixProbe = Join-Path $BuildDir "amd_vulkan_matrix_probe.exe"
$MixedDotProbe = Join-Path $BuildDir "amd_vulkan_mixed_dot_probe.exe"
$MathVerify = Join-Path $BuildDir "amd_vulkan_math_verify.exe"
$CandidateVerify = Join-Path $BuildDir "amd_vulkan_candidate_verify.exe"
$SpirvDis = Join-Path $VulkanSdk "Bin/spirv-dis.exe"

Write-Host "[AMD SPIR-V] Runtime math semantic verification"
$SemanticDir = Join-Path $OutDir "semantic"
New-Item -ItemType Directory -Force -Path $SemanticDir | Out-Null
$SemanticSpv = Join-Path $SemanticDir "math_execute.spv"
$SemanticShader = Join-Path $ProbeRoot "shaders/math_execute.hlsl"
& $Dxc -spirv -T cs_6_2 -E main -fspv-target-env=vulkan1.2 -fspv-extension=AMD `
    $SemanticShader -Fo $SemanticSpv
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $MathVerify $SemanticSpv
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] SAD/MSAD native recovery"
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
python @SadArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Packed integer + packed-half FP16 dot native recovery"
$DotArgs = @(
    (Join-Path $ProbeRoot "dot_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "dot")
)
python @DotArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Saturating dot direct/canonical A/B"
$DotSatArgs = @(
    (Join-Path $ProbeRoot "dot_saturation_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "dot-saturation")
)
python @DotSatArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Exact FP16/BF16 mixed-dot qualification"
$MixedDotArgs = @(
    (Join-Path $ProbeRoot "mixed_dot_probe.py"),
    "--dxc", $Dxc,
    "--mixed-dot-probe", $MixedDotProbe,
    "--out-dir", (Join-Path $OutDir "mixed-dot")
)
python @MixedDotArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# dot_f16_probe.py intentionally remains a separate manual probe. The mixed-dot
# qualification above is now the stronger exact SPV_VALVE path when supported;
# this older probe remains useful as a canonical-recovery control.

Write-Host "[AMD SPIR-V] APUSR cross-lane native recovery"
$CrosslaneArgs = @(
    (Join-Path $ProbeRoot "crosslane_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "crosslane")
)
python @CrosslaneArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Wave reduction direct/butterfly A/B"
$ReductionArgs = @(
    (Join-Path $ProbeRoot "reduction_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "reduction")
)
python @ReductionArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Half conversion scalar/packed A/B"
$ConversionArgs = @(
    (Join-Path $ProbeRoot "conversion_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "half-conversion")
)
python @ConversionArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Packed normalized/integer conversion recovery"
$PackingArgs = @(
    (Join-Path $ProbeRoot "packing_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "packing")
)
python @PackingArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Cooperative matrix property + ISA qualification"
$MatrixArgs = @(
    (Join-Path $ProbeRoot "matrix_probe.py"),
    "--dxc", $Dxc,
    "--matrix-probe", $MatrixProbe,
    "--out-dir", (Join-Path $OutDir "matrix")
)
python @MatrixArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Transcendental spelling A/B"
$TranscendentalArgs = @(
    (Join-Path $ProbeRoot "transcendental_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "transcendental")
)
python @TranscendentalArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] LDS scalar/vector access A/B"
$LdsArgs = @(
    (Join-Path $ProbeRoot "lds_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "lds")
)
python @LdsArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Cube helper native recovery"
$CubeArgs = @(
    (Join-Path $ProbeRoot "cube_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "cube")
)
python @CubeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Empirical candidate zoo + runtime oracle"
$ZooArgs = @(
    (Join-Path $ProbeRoot "candidate_zoo.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--runtime-verifier", $CandidateVerify,
    "--out-dir", (Join-Path $OutDir "candidate-zoo")
)
python @ZooArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Core math native recovery"
$MathArgs = @(
    (Join-Path $ProbeRoot "math_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "math")
)
python @MathArgs
exit $LASTEXITCODE
