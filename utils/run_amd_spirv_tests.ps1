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

$ContractChunks = @(
    @("Core SPIR-V", "utils/amd_spirv_ci.py"),
    @("Low-level math", "utils/amd_math_ci.py"),
    @("Subgroup/cross-lane", "utils/amd_subgroup_ci.py"),
    @("64-bit atomics", "utils/amd_atomic_ci.py"),
    @("Packed conversions", "utils/amd_packing_ci.py"),
    @("Cooperative matrices", "utils/amd_matrix_ci.py"),
    @("Mixed-precision dot", "utils/amd_mixed_dot_ci.py"),
    @("DXIL parity", "utils/amd_dxil_ci.py")
)

foreach ($Chunk in $ContractChunks) {
    Write-Host ("[AMD contracts] " + $Chunk[0])
    python (Join-Path $RepoRoot $Chunk[1]) --dxc $DxcPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $NativeProbe) {
    Write-Host "[AMD contracts] PASS all compiler contract chunks"
    Write-Host "[AMD contracts] Hardware ISA probe skipped (pass -NativeProbe when native recovery matters)."
    exit 0
}

if (-not $VulkanSdk) {
    throw "VULKAN_SDK is not set. Install/activate a Vulkan SDK or pass -VulkanSdk for -NativeProbe."
}

Write-Host "[AMD contracts] Installed-driver ISA qualification"
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
