# Build the checked-out project and update the local installation.
# Windows PowerShell 5.1 and PowerShell 7; no administrator session required.
[CmdletBinding()]
param(
    [string]$InstallDir = '',
    [ValidateRange(1, 64)][int]$Jobs = [Math]::Min(4, [Environment]::ProcessorCount),
    [switch]$NoLaunch
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$sourceRoot = $PSScriptRoot
$transcribing = $false
$packageOutputPrepared = $false
$originalPath = $env:PATH
$originalLocation = Get-Location

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

try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or -not [Environment]::Is64BitOperatingSystem) {
        throw 'This installer requires Windows x64.'
    }
    if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { throw 'This build currently requires an x64 PC.' }
    Set-Location -LiteralPath $sourceRoot
    $logDir = Join-Path $sourceRoot 'build\passthrough-install'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $logPath = Join-Path $logDir ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-build.log')
    Start-Transcript -Path $logPath | Out-Null
    $transcribing = $true
    Assert-VentureStopped

    $git = Get-Command git.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $git) {
        foreach ($candidate in @("$env:ProgramFiles\Git\cmd", "$env:LOCALAPPDATA\Programs\Git\cmd")) {
            if (Test-Path -LiteralPath (Join-Path $candidate 'git.exe')) { $env:PATH = "$candidate;$env:PATH"; break }
        }
        $git = Get-Command git.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if ($null -eq $git) { throw 'Git for Windows is required. Install it, then run this script again.' }
    $revision = & $git.Source rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Run this installer from the cloned Venture fork.' }
    & $git.Source diff --quiet HEAD --
    if ($LASTEXITCODE -eq 1) { $revision = "$revision-dirty" }
    elseif ($LASTEXITCODE -ne 0) { throw 'Could not inspect checkout changes.' }

    $python = Find-BuildPython
    if (-not $python) {
        $winget = Get-Command winget.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $winget) { throw 'Python 3.13+ x64 is required. Install Python, then run this script again.' }
        Invoke-BuildStep 'Install Python 3.13 for the current user' $winget.Source @(
            'install', '--id', 'Python.Python.3.13', '--exact', '--source', 'winget', '--scope', 'user',
            '--architecture', 'x64', '--silent', '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity'
        )
        $python = Find-BuildPython
        if (-not $python) { throw 'Python was installed but could not be found. Open a new terminal and rerun the script.' }
    }
    # Let depot_tools and any build subprocess find the same interpreter.
    $env:PATH = "$(Split-Path -Parent $python);$env:PATH"
    Write-Host "Building revision $revision with $python"
    Write-Host 'The first build downloads and compiles portable dependencies. Later runs reuse their caches.'
    Invoke-BuildStep 'Prepare toolchain, JDK, FFmpeg and compiler cache' $python @('tools/bootstrap.py', '--jdk', '--ccache')
    Invoke-BuildStep 'Prepare ANGLE' $python @('tools/angle/build_angle.py', '--jobs', [string]$Jobs)

    $cmake = Join-Path $sourceRoot '.deps\cmake\bin\cmake.exe'
    $ctest = Join-Path $sourceRoot '.deps\cmake\bin\ctest.exe'
    Invoke-BuildStep 'Configure Venture' $cmake @('--preset', 'windows-release', '-DVENTURE_BUILD_TESTS=ON', '-DVENTURE_AGENT_CACHE=ON')
    Invoke-BuildStep 'Prepare clean release package' $python @('tools/packaging/clean_preview_package.py')
    $packageOutputPrepared = $true
    Invoke-BuildStep 'Compile and package Venture' $cmake @('--build', '--preset', 'windows-release', '--parallel', [string]$Jobs,
        '--target', 'package-release', 'venture_framework_tests')
    Invoke-BuildStep 'Check OpenXR metadata and Unity permissions' $ctest @('--preset', 'windows-release',
        '-R', '^(xr_codegen|framework_fragment_permissions)$', '--output-on-failure')
    Invoke-BuildStep 'Check preview installation' $python @('tests/install_preview_test.py')

    if ([string]::IsNullOrWhiteSpace($InstallDir)) {
        $InstallDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Venture-Passthrough'
    }
    $layout = Join-Path $sourceRoot 'dist\Venture-win64'
    Assert-VentureStopped
    Invoke-BuildStep 'Install verified test build' $python @(
        'tools/packaging/install_preview.py', '--layout', $layout, '--install-root', $InstallDir, '--revision', $revision
    )
    $receiptPath = Join-Path $InstallDir 'current.json'
    $installed = (Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json).directory
    $starter = Join-Path $installed 'venture.exe'

    $shell = New-Object -ComObject WScript.Shell
    $desktop = [Environment]::GetFolderPath('DesktopDirectory')
    if (-not [string]::IsNullOrWhiteSpace($desktop) -and (Test-Path -LiteralPath $desktop)) {
        $shortcut = $shell.CreateShortcut((Join-Path $desktop 'Venture Passthrough.lnk'))
        $shortcut.TargetPath = $starter
        $shortcut.Arguments = ''
        $shortcut.WorkingDirectory = $installed
        $shortcut.IconLocation = (Join-Path $installed 'venture.exe') + ',0'
        $shortcut.Save()
    }
    Write-Host "`nInstalled: $installed" -ForegroundColor Green
    Write-Host "Start: $starter"
    if (-not $NoLaunch) {
        $running = @(Get-Process -Name 'venture', 'venture-runtime' -ErrorAction SilentlyContinue)
        if ($running.Count -gt 0) {
            Write-Host 'Close the running Venture/game, then use the Venture Passthrough desktop shortcut.'
        } else {
            Start-Process -FilePath $starter -WorkingDirectory $installed
        }
    }
    Write-Host "Build log: $logPath"
} catch {
    Write-Host "`nBuild/install failed: $($_.Exception.Message)" -ForegroundColor Red
    if (Get-Variable logPath -ErrorAction SilentlyContinue) { Write-Host "Build log: $logPath" }
    exit 1
} finally {
    if ($packageOutputPrepared) {
        try {
            Invoke-BuildStep 'Remove generated release package' $python @((Join-Path $sourceRoot 'tools\packaging\clean_preview_package.py'))
        } catch {
            Write-Host "Could not remove the generated release package: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
    $env:PATH = $originalPath
    Set-Location -LiteralPath $originalLocation.Path
    if ($transcribing) { Stop-Transcript | Out-Null }
}
