param(
    [Parameter(Mandatory=$true)][string]$Dxc,
    [string]$Rga,
    [string]$RgaTarget,
    [string]$VulkanSdk = $env:VULKAN_SDK,
    [string]$OutDir = "out/amd-sad-native-probe",
    [switch]$RgaLive
)

$ErrorActionPreference = "Stop"
$ProbeRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $OutDir "driver-probe-build"

if (-not $VulkanSdk) {
    throw "VULKAN_SDK is not set. Install/activate a Vulkan SDK or pass -VulkanSdk."
}

cmake -G Ninja -S $ProbeRoot -B $BuildDir -DCMAKE_BUILD_TYPE=Release -DVulkan_ROOT="$VulkanSdk"
cmake --build $BuildDir

$DriverProbe = Join-Path $BuildDir "amd_vulkan_isa_probe.exe"
$SpirvDis = Join-Path $VulkanSdk "Bin/spirv-dis.exe"

$Args = @(
    (Join-Path $ProbeRoot "native_probe.py"),
    "--dxc", $Dxc,
    "--driver-probe", $DriverProbe,
    "--spirv-dis", $SpirvDis,
    "--out-dir", $OutDir
)
if ($Rga) { $Args += @("--rga", $Rga) }
if ($RgaTarget) { $Args += @("--rga-target", $RgaTarget) }
if ($RgaLive) { $Args += "--rga-live" }

python @Args
exit $LASTEXITCODE
