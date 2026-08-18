param(
    [Parameter(Mandatory=$true)][string]$Dxc,
    [switch]$NativeProbe,
    [string]$Rga,
    [string]$RgaTarget,
    [string]$VulkanSdk = $env:VULKAN_SDK,
    [string]$OutDir = "out/amd-spirv-validation",
    [switch]$RgaLive
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$DxcPath = (Resolve-Path $Dxc).Path

Write-Host "[AMD SPIR-V] Contract regressions"
python (Join-Path $RepoRoot "utils/amd_spirv_ci.py") --dxc $DxcPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[AMD SPIR-V] Low-level math regressions"
python (Join-Path $RepoRoot "utils/amd_math_ci.py") --dxc $DxcPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $NativeProbe) {
    Write-Host "[AMD SPIR-V] PASS contract + low-level math regressions"
    Write-Host "[AMD SPIR-V] Hardware ISA probe skipped (pass -NativeProbe when native recovery matters)."
    exit 0
}

if (-not $VulkanSdk) {
    throw "VULKAN_SDK is not set. Install/activate a Vulkan SDK or pass -VulkanSdk for -NativeProbe."
}

Write-Host "[AMD SPIR-V] Installed-driver ISA probe"
$Probe = Join-Path $RepoRoot "utils/amd_vulkan_isa_probe/run_windows.ps1"
$ProbeArgs = @(
    "-Dxc", $DxcPath,
    "-VulkanSdk", $VulkanSdk,
    "-OutDir", (Join-Path $OutDir "native")
)
if ($Rga) { $ProbeArgs += @("-Rga", $Rga) }
if ($RgaTarget) { $ProbeArgs += @("-RgaTarget", $RgaTarget) }
if ($RgaLive) { $ProbeArgs += "-RgaLive" }

& $Probe @ProbeArgs
exit $LASTEXITCODE
