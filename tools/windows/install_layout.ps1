# Only the final file installation runs with administrator rights when needed.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Layout,
    [Parameter(Mandatory = $true)][string]$InstallDir,
    [Parameter(Mandatory = $true)][string]$Revision,
    [Parameter(Mandatory = $true)][string]$LogPath
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'build_helpers.ps1')
$transcribing = $false
$installExit = 0
try {
    Start-Transcript -Path $LogPath | Out-Null
    $transcribing = $true
    Assert-VentureStopped
    $starter = Install-VentureLayout $Layout $InstallDir $Revision
    Write-Host "Installed: $starter"
} catch {
    Write-Host "Installation failed: $($_.Exception.Message)" -ForegroundColor Red
    $installExit = 1
} finally {
    if ($transcribing) { Stop-Transcript | Out-Null }
}
exit $installExit
