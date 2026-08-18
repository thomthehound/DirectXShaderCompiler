param(
    [Parameter(Mandatory=$true)][string]$Dxc,
    [string]$ApusrRoot,
    [ValidateSet("inventory", "compiler", "smoke", "qualify")][string]$Tier = "compiler",
    [string]$Family = "all",
    [switch]$NativeProbe,
    [string]$VulkanSdk = $env:VULKAN_SDK
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$DxcPath = (Resolve-Path $Dxc).Path

$Args = @(
    (Join-Path $RepoRoot "utils/apusr_amd_qualify.py"),
    "--tier", $Tier,
    "--family", $Family
)

if ($Tier -ne "inventory") {
    $Args += @("--dxc", $DxcPath)
}

if ($ApusrRoot) {
    $Args += @("--apusr-root", (Resolve-Path $ApusrRoot).Path)
}

if ($NativeProbe) {
    $Args += "--native-probe"
    if ($VulkanSdk) {
        $Args += @("--vulkan-sdk", $VulkanSdk)
    }
}

python @Args
exit $LASTEXITCODE
