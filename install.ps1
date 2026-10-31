# Build and install the checked-out Venture source using its pinned toolchain.
# Compatible with Windows PowerShell 5.1 and PowerShell 7.
[CmdletBinding()]
param(
    [string]$InstallDir = '',
    [ValidateRange(1, 64)][int]$Jobs = [Math]::Min(4, [Environment]::ProcessorCount),
    [switch]$RunTests,
    [switch]$NoLaunch
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$sourceRoot = $PSScriptRoot
$originalPath = $env:PATH
$originalLocation = Get-Location
$transcribing = $false
$package = $null
. (Join-Path $sourceRoot 'tools\windows\build_helpers.ps1')

function Find-BuildPython {
    $candidates = @()
    foreach ($name in @('py.exe', 'python.exe', 'python3.exe')) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $command -and $command.Source -notlike '*\Microsoft\WindowsApps\*') {
            $prefix = @()
            if ($name -eq 'py.exe') { $prefix = @('-3') }
            $candidates += [pscustomobject]@{ Path = $command.Source; Prefix = $prefix }
        }
    }
    foreach ($version in @('314', '313')) {
        $candidate = Join-Path $env:LOCALAPPDATA "Programs\Python\Python$version\python.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $candidates += [pscustomobject]@{ Path = $candidate; Prefix = @() }
        }
    }
    foreach ($candidate in $candidates) {
        $prefix = $candidate.Prefix
        $check = & $candidate.Path @prefix -c 'import struct,sys; print(sys.executable); sys.exit(0 if sys.version_info >= (3,13) and struct.calcsize(chr(80))==8 else 1)' 2>$null
        if ($LASTEXITCODE -eq 0 -and $check) { return [string]($check | Select-Object -Last 1) }
    }
    return $null
}


function Find-NativeBuildGnuPG {
    $directories = @()
    foreach ($name in @('gpg.exe', 'gpgv.exe')) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $command) { $directories += Split-Path -Parent $command.Source }
    }
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA)) {
        if ($root) { $directories += Join-Path $root 'GnuPG\bin' }
    }
    foreach ($directory in $directories) {
        if ((Test-Path -LiteralPath (Join-Path $directory 'gpg.exe')) -and
            (Test-Path -LiteralPath (Join-Path $directory 'gpgv.exe')) -and
            -not (Test-Path -LiteralPath (Join-Path $directory 'msys-2.0.dll'))) { return $directory }
    }
    return $null
}

try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or
        -not [Environment]::Is64BitOperatingSystem -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
        throw 'This build/install script requires Windows x64.'
    }
    Set-Location -LiteralPath $sourceRoot
    $logDir = Join-Path $sourceRoot 'build\local-install'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $logPath = Join-Path $logDir ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-build.log')
    Start-Transcript -Path $logPath | Out-Null
    $transcribing = $true
    Assert-VentureStopped
    $git = Get-Command git.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $revision = & $git.Source rev-parse HEAD
    if ($LASTEXITCODE) { throw 'Run this script from a cloned Venture checkout.' }
    & $git.Source diff --quiet HEAD --
    if ($LASTEXITCODE -eq 1) { $revision = "$revision-dirty" }
    elseif ($LASTEXITCODE) { throw 'Could not inspect checkout changes.' }
    $python = Find-BuildPython
    if (-not $python) {
        $winget = Get-Command winget.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
        Invoke-BuildStep 'Install Python 3.13 for the current user' $winget.Source @(
            'install', '--id', 'Python.Python.3.13', '--exact', '--source', 'winget', '--scope', 'user',
            '--architecture', 'x64', '--silent', '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity')
        $python = Find-BuildPython
        if (-not $python) { throw 'Python 3.13+ x64 could not be found after installation.' }
    }
    $env:PATH = "$(Split-Path -Parent $python);$env:PATH"
    Write-Host "Building revision $revision"
    Write-Host 'The first build downloads and compiles dependencies. Later builds reuse toolchain, ANGLE and compiler caches.'
    # A valid authenticated FFmpeg cache needs no verifier installation.
    $probe = @'
import sys
from tools.ffmpeg import build_ffmpeg as ffmpeg
try: ffmpeg.validate_cache(ffmpeg.BASE)
except (OSError, ValueError, KeyError, RuntimeError): sys.exit(1)
'@
    & $python -c $probe
    if ($LASTEXITCODE) {
        $gnupg = Find-NativeBuildGnuPG
        if (-not $gnupg) {
            $winget = Get-Command winget.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
            Write-Host 'Native GnuPG is needed to verify FFmpeg. Windows may request permission to install it.'
            Invoke-BuildStep 'Install the native FFmpeg signature verifier' $winget.Source @(
                'install', '--id', 'GnuPG.GnuPG', '--exact', '--source', 'winget', '--silent',
                '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity')
            $gnupg = Find-NativeBuildGnuPG
            if (-not $gnupg) { throw 'Native GnuPG was installed but gpg.exe and gpgv.exe could not be found.' }
        }
        $env:PATH = "$gnupg;$env:PATH"
    }
    Invoke-BuildStep 'Prepare pinned toolchain, JDK, FFmpeg and compiler cache' $python @('tools/bootstrap.py', '--jdk', '--ccache')
    Invoke-BuildStep 'Prepare ANGLE' $python @('tools/angle/build_angle.py', '--jobs', [string]$Jobs)
    $cmake = Join-Path $sourceRoot '.deps\cmake\bin\cmake.exe'
    $testSetting = if ($RunTests) { 'ON' } else { 'OFF' }
    Invoke-BuildStep 'Configure Venture' $cmake @('--preset', 'windows-release', "-DVENTURE_BUILD_TESTS=$testSetting", '-DVENTURE_AGENT_CACHE=ON')
    $targets = @('venture', 'venture-runtime')
    if ($RunTests) { $targets = @('all') }
    Invoke-BuildStep 'Compile Venture' $cmake (@('--build', '--preset', 'windows-release', '--parallel', [string]$Jobs, '--target') + $targets)
    if ($RunTests) {
        Invoke-BuildStep 'Run the configured tests' (Join-Path $sourceRoot '.deps\cmake\bin\ctest.exe') @('--preset', 'windows-release', '--output-on-failure')
    }
    $cache = Join-Path $sourceRoot 'build\windows-release\CMakeCache.txt'
    $angle = Get-VentureCMakeValue $cache 'VENTURE_ANGLE_ROOT'
    $strip = Get-VentureCMakeValue $cache 'CMAKE_STRIP'
    $compiler = Get-VentureCMakeValue $cache 'CMAKE_CXX_COMPILER'
    $readobj = Join-Path (Split-Path -Parent $compiler) 'llvm-readobj.exe'
    $package = Join-Path $logDir ('package-' + [Guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $package) { throw 'The fresh package path unexpectedly exists.' }
    Invoke-BuildStep 'Create the ordinary verified release layout' $python @('tools/package_release.py',
        '--build', 'build/windows-release', '--output', $package, '--angle', $angle, '--strip', $strip, '--readobj', $readobj)
    $directoryName = 'Venture-Local'
    $shortcutName = 'Venture Local'
    $profile = Join-Path $sourceRoot '.venture-install.json'
    if (Test-Path -LiteralPath $profile -PathType Leaf) {
        $configuration = Get-Content -LiteralPath $profile -Raw | ConvertFrom-Json
        $directoryName = [string]$configuration.installDirectoryName
        $shortcutName = [string]$configuration.shortcutName
        if ($directoryName -notmatch '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$' -or
            $shortcutName -notmatch '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$') { throw 'Invalid local-install profile names.' }
    }
    if ([string]::IsNullOrWhiteSpace($InstallDir)) {
        $InstallDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) $directoryName
    }
    Assert-VentureStopped
    $starter = Install-VentureLayout $package $InstallDir $revision
    $desktop = [Environment]::GetFolderPath('DesktopDirectory')
    if ($desktop -and (Test-Path -LiteralPath $desktop)) {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut((Join-Path $desktop ($shortcutName + '.lnk')))
        $shortcut.TargetPath = $starter
        $shortcut.WorkingDirectory = $InstallDir
        $shortcut.IconLocation = $starter + ',0'
        $shortcut.Save()
    }
    Write-Host "Installed: $InstallDir" -ForegroundColor Green
    if (-not $NoLaunch) { Start-Process -FilePath $starter -WorkingDirectory $InstallDir }
    Write-Host "Build log: $logPath"
} catch {
    Write-Host "Build/install failed: $($_.Exception.Message)" -ForegroundColor Red
    if (Get-Variable logPath -ErrorAction SilentlyContinue) { Write-Host "Build log: $logPath" }
    exit 1
} finally {
    if ($null -ne $package) {
        try { Remove-VentureBuildPackage $package $logDir }
        catch { Write-Host "Could not remove generated package: $($_.Exception.Message)" -ForegroundColor Yellow }
    }
    $env:PATH = $originalPath
    Set-Location -LiteralPath $originalLocation.Path
    if ($transcribing) { Stop-Transcript | Out-Null }
}
