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
    Write-Host 'PASS Program Files defaults, explicit paths, literal worker arguments, elevation routing and failure handling'
} finally {
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
