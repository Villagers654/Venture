Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\tools\windows\build_helpers.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('venture-install-rights-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
function Check([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
function Refuses([scriptblock]$Action) {
    $failed = $false
    try { $null = & $Action } catch { $failed = $true }
    Check $failed 'An invalid or failed installation was accepted.'
}
function Layout([string]$Name) {
    $root = Join-Path $fixture $Name
    New-Item -ItemType Directory -Path $root | Out-Null
    $files = @{}
    foreach ($name in @('venture.exe', 'venture-runtime.exe')) {
        $path = Join-Path $root $name
        [IO.File]::WriteAllText($path, 'fixture:' + $name)
        $files[$name] = @{bytes = (Get-Item -LiteralPath $path).Length; sha256 = (Get-FileHash -LiteralPath $path).Hash}
    }
    Write-VentureInstallState (Join-Path $root 'release-files.json') @{files = $files}
    return $root
}
try {
    $source = Layout 'source with spaces'
    $destination = Join-Path $fixture 'installed'
    $config = Get-VentureInstallConfiguration $fixture ($destination + [IO.Path]::DirectorySeparatorChar)
    Check ($config.InstallDir -eq $destination -and $config.ShortcutName -eq 'Venture') 'Explicit path or default shortcut is wrong.'
    Check (Test-VentureInstallWritable $destination) 'Writable destination requires elevation.'
    Check (@(Get-ChildItem -LiteralPath $fixture -Filter 'venture-access-*' -Force).Count -eq 0) 'The write probe leaked a temporary file.'
    $starter = Install-VenturePackage $source $destination 'ready' '' ''
    Check (Test-Path -LiteralPath $starter) 'Writable installation failed.'
    Check ((Join-VentureProcessArguments @('C:\Program Files\Venture', 'C:\tail\')) -eq '"C:\Program Files\Venture" "C:\tail\\"') 'Paths with spaces or trailing backslashes are not quoted correctly.'
    Refuses { Join-VentureProcessArguments @('unexpected"quote') }

    $legacy = Join-Path $fixture 'previous'
    $null = Install-VentureLayout $source $legacy 'old'
    [IO.File]::WriteAllText((Join-Path $legacy 'my-notes.txt'), 'user file')
    Write-VentureInstallState (Join-Path $destination '.venture-local-install.json') @{owner = 'venture-local-build'; status = 'updating'}
    Check (-not (Remove-VentureLegacyInstall $legacy $destination)) 'Legacy files were removed before the new install completed.'
    $null = Install-VentureLayout $source $destination 'ready'
    Check (Remove-VentureLegacyInstall $legacy $destination) 'Owned old installation was not cleaned up.'
    Check (-not (Test-Path -LiteralPath (Join-Path $legacy 'venture.exe'))) 'An old executable was retained.'
    Check ((Get-Content -LiteralPath (Join-Path $legacy 'my-notes.txt') -Raw) -eq 'user file') 'Legacy cleanup removed a user file.'
    Check (-not (Remove-VentureLegacyInstall $destination ($destination + [IO.Path]::DirectorySeparatorChar))) 'Cleanup accepted the current installation as a legacy path.'
    Check (Test-Path -LiteralPath $starter) 'Cleanup removed the current executable.'
    $modified = Join-Path $fixture 'modified'
    $null = Install-VentureLayout $source $modified 'old'
    [IO.File]::WriteAllText((Join-Path $modified 'venture.exe'), 'user modification')
    Check (-not (Remove-VentureLegacyInstall $modified $destination)) 'Modified legacy files were removed.'
    Check (Test-Path -LiteralPath (Join-Path $modified 'venture-runtime.exe')) 'Legacy cleanup deleted files before validating the whole manifest.'
    $unowned = Layout 'unowned'
    Check (-not (Remove-VentureLegacyInstall $unowned $destination)) 'An unowned installation was removed.'

    $emptyLegacy = Join-Path $fixture 'empty-legacy'
    $null = Install-VentureLayout $source $emptyLegacy 'old'
    Check (Remove-VentureLegacyInstall $emptyLegacy $destination) 'Unchanged legacy layout was retained.'
    Check (-not (Test-Path -LiteralPath $emptyLegacy)) 'Empty legacy directory was retained.'
    $linked = Join-Path $fixture 'linked-legacy'
    $null = Install-VentureLayout $source $linked 'old'
    $outside = Layout 'outside'
    Remove-Item -LiteralPath (Join-Path $linked 'venture.exe') -Force
    # A directory junction/symlink is sufficient to exercise path refusal on
    # every Windows runner without enabling developer-mode file symlinks.
    $nested = Join-Path $linked 'redirect'
    $directoryLinkType = if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) { 'Junction' } else { 'SymbolicLink' }
    New-Item -ItemType $directoryLinkType -Path $nested -Target $outside | Out-Null
    $manifest = Get-Content -LiteralPath (Join-Path $linked 'release-files.json') -Raw | ConvertFrom-Json
    $manifest.files | Add-Member -NotePropertyName 'redirect/venture.exe' -NotePropertyValue @{bytes = 0; sha256 = ('0' * 64)}
    Write-VentureInstallState (Join-Path $linked 'release-files.json') $manifest
    Refuses { Remove-VentureLegacyInstall $linked $destination }
    Check ((Get-Content -LiteralPath (Join-Path $outside 'venture.exe') -Raw) -eq 'fixture:venture.exe') 'Legacy cleanup followed a junction.'
    Remove-Item -LiteralPath $nested -Force

    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        $nativeProgramFiles = [Environment]::GetEnvironmentVariable('ProgramW6432')
        if (-not $nativeProgramFiles) { $nativeProgramFiles = [Environment]::GetFolderPath('ProgramFiles') }
        $config = Get-VentureInstallConfiguration $fixture ''
        Check ($config.InstallDir -eq (Join-Path $nativeProgramFiles 'Venture')) 'Default is not native Program Files\Venture.'
        Write-VentureInstallState (Join-Path $fixture '.venture-install.json') @{installDirectoryName = 'Venture Custom'; shortcutName = 'Custom'}
        $config = Get-VentureInstallConfiguration $fixture ''
        Check ($config.InstallDir -eq (Join-Path $nativeProgramFiles 'Venture Custom') -and $config.ShortcutName -eq 'Custom') 'Profile was not resolved under Program Files.'

        # Keep the real process/argv/worker path, replacing only the interactive
        # RunAs launch so CI does not display a UAC prompt.
        $workerDir = Join-Path $fixture 'worker space $ & apostrophe''s'
        New-Item -ItemType Directory -Path $workerDir | Out-Null
        foreach ($name in @('build_helpers.ps1', 'install_layout.ps1')) {
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot ("..\tools\windows\" + $name)) -Destination $workerDir
        }
        $worker = Join-Path $workerDir 'install_layout.ps1'
        $target = Join-Path $fixture 'Program Files $ & apostrophe''s\Venture'
        $log = Join-Path $workerDir 'install transcript.log'
        $writable = ${function:Test-VentureInstallWritable}
        function Test-VentureInstallWritable([string]$InstallDir) { return $false }
        $script:sawElevation = $false
        function Start-Process {
            param([string]$FilePath, [string]$Verb, [string]$ArgumentList, [switch]$Wait, [switch]$PassThru, [string]$ErrorAction)
            Check ($Verb -eq 'RunAs' -and $Wait -and $PassThru) 'Installation did not request bounded administrator execution.'
            $script:sawElevation = $true
            Microsoft.PowerShell.Management\Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -Wait -PassThru
        }
        try {
            $starter = Install-VenturePackage $source $target 'literal-revision' $worker $log
            Check $script:sawElevation 'Protected destination did not request elevation.'
            Check ((Get-Content -LiteralPath $starter -Raw) -eq 'fixture:venture.exe') 'Worker lost literal path arguments.'
            Check ((Get-Content -LiteralPath $log -Raw).Contains('Installed:')) 'Worker installation transcript is missing.'
            Add-Content -LiteralPath (Join-Path $source 'venture.exe') -Value 'tampered'
            Refuses { Install-VenturePackage $source $target 'failed' $worker $log }
            Check ((Get-Content -LiteralPath $starter -Raw) -eq 'fixture:venture.exe') 'Worker failure corrupted the installed executable.'
            Check ((Get-Content -LiteralPath $log -Raw).Contains('Installation failed:')) 'Worker failure was omitted from its transcript.'
        } finally {
            Remove-Item Function:Start-Process
            Set-Item Function:Test-VentureInstallWritable $writable
        }
        function Test-VentureInstallWritable([string]$InstallDir) { return $false }
        function Start-Process { throw [ComponentModel.Win32Exception]::new(1223) }
        try { Refuses { Install-VenturePackage $source $target 'cancelled' $worker $log } }
        finally {
            Remove-Item Function:Start-Process
            Set-Item Function:Test-VentureInstallWritable $writable
        }
        Check ((Get-Content -LiteralPath $starter -Raw) -eq 'fixture:venture.exe') 'Cancelled elevation changed the installation.'
    }
    Write-Host 'PASS Program Files defaults, explicit paths, literal worker arguments, elevation routing/failure and owned legacy cleanup'
} finally {
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
