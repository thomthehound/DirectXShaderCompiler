param(
    [string]$Dxc,
    [string]$ApusrRoot,
    [ValidateSet("inventory", "compiler", "smoke", "qualify")][string]$Tier = "compiler",
    [string]$Family = "all",
    [switch]$NativeProbe,
    [string]$VulkanSdk = $env:VULKAN_SDK
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

Write-Host "[APUSR AMD] Empirical candidate inventory"
python (Join-Path $RepoRoot "utils/apusr_amd_experimental_inventory.py")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Args = @(
    (Join-Path $RepoRoot "utils/apusr_amd_qualify.py"),
    "--tier", $Tier,
    "--family", $Family
)

if ($Tier -ne "inventory") {
    if (-not $Dxc) {
        throw "-Dxc is required for compiler/smoke/qualify tiers."
    }
    $DxcPath = (Resolve-Path $Dxc).Path

    Write-Host "[APUSR AMD] Exact subgroup contracts"
    python (Join-Path $RepoRoot "utils/amd_subgroup_ci.py") --dxc $DxcPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Host "[APUSR AMD] DXIL AMD contracts"
    python (Join-Path $RepoRoot "utils/amd_dxil_ci.py") --dxc $DxcPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $Args += @("--dxc", $DxcPath)
}

if ($ApusrRoot) {
    $Args += @("--apusr-root", (Resolve-Path $ApusrRoot).Path)
}

if ($NativeProbe) {
    if ($Tier -eq "inventory") {
        throw "-NativeProbe requires compiler, smoke, or qualify tier."
    }
    $Args += "--native-probe"
    if ($VulkanSdk) {
        $Args += @("--vulkan-sdk", $VulkanSdk)
    }
}

python @Args
exit $LASTEXITCODE
