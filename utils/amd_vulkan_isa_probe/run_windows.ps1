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
$MathVerify = Join-Path $BuildDir "amd_vulkan_math_verify.exe"
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

Write-Host "[AMD SPIR-V] Packed integer dot native recovery"
$DotArgs = @(
    (Join-Path $ProbeRoot "dot_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "dot")
)
python @DotArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] FP16 dot native recovery"
$DotF16Args = @(
    (Join-Path $ProbeRoot "dot_f16_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "dot-f16")
)
python @DotF16Args
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] APUSR cross-lane native recovery"
$CrosslaneArgs = @(
    (Join-Path $ProbeRoot "crosslane_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--out-dir", (Join-Path $OutDir "crosslane")
)
python @CrosslaneArgs
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
