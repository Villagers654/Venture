function Assert-VentureStopped {
    $running = @(Get-Process -Name 'venture', 'venture-runtime' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw 'Close Venture and its running game, then rerun the installer. The application files are updated in place.'
    }
}

# Native stderr must reach the PowerShell host to be included in transcripts.
# In Windows PowerShell 5.1, redirected stderr becomes ErrorRecord objects.
function Invoke-BuildStep {
    param(
        [string]$Label,
        [string]$Executable,
        [string[]]$Arguments
    )
    Write-Host "`n== $Label ==" -ForegroundColor Cyan
    $command = Get-Command $Executable -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $invocationFailure = $null
    $previousPreference = $ErrorActionPreference
    try {
        # Stderr is build output, even with the installer's Stop preference.
        # Judge the child by its exit code after draining both streams.
        $ErrorActionPreference = 'Continue'
        $PSNativeCommandUseErrorActionPreference = $false
        & $command.Source @Arguments 2>&1 | ForEach-Object {
            $line = $_.ToString()
            Write-Host $line
            if ($_ -is [System.Management.Automation.ErrorRecord] -and
                $_.FullyQualifiedErrorId -notlike 'NativeCommandError*') {
                $invocationFailure = $_
            }
        }
        $stepExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($null -ne $invocationFailure) { throw "$Label could not run: $invocationFailure" }
    if ($stepExitCode -ne 0) { throw "$Label failed (exit $stepExitCode)." }
}

function Assert-NoVentureReparsePoint([string]$Path) {
    $part = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($part)) {
        $item = Get-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing a link or junction in build/install path: $part"
        }
        $parent = [IO.Path]::GetDirectoryName($part)
        if ($parent -eq $part) { break }
        $part = $parent
    }
}

function Get-VentureManagedPath([string]$Root, [string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Name) -or [IO.Path]::IsPathRooted($Name) -or $Name.Contains(':')) {
        throw "Invalid release file name: $Name"
    }
    $segments = $Name -split '[\\/]'
    if (@($segments | Where-Object { $_ -eq '..' -or $_ -eq '.' -or $_ -eq '' }).Count) {
        throw "Invalid release file name: $Name"
    }
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
    $result = [IO.Path]::GetFullPath((Join-Path $Root ($segments -join [IO.Path]::DirectorySeparatorChar)))
    if (-not $result.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Release file escapes its directory.' }
    Assert-NoVentureReparsePoint $result
    return $result
}

function Get-VentureManifest([string]$Root) {
    $path = Get-VentureManagedPath $Root 'release-files.json'
    $manifest = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $files = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($property in $manifest.files.PSObject.Properties) {
        $null = Get-VentureManagedPath $Root $property.Name
        if ($files.ContainsKey($property.Name)) { throw 'Release manifest has duplicate file names.' }
        $entry = $property.Value
        if ([long]$entry.bytes -lt 0 -or [string]$entry.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid release file metadata.' }
        $files.Add($property.Name, [pscustomobject]@{ Bytes = [long]$entry.bytes; Sha256 = [string]$entry.sha256 })
    }
    if (-not $files.ContainsKey('venture.exe') -or -not $files.ContainsKey('venture-runtime.exe')) {
        throw 'The release manifest must include both Venture executables.'
    }
    return ,$files
}

function Get-VentureTreeFiles([string]$Root) {
    Assert-NoVentureReparsePoint $Root
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push([IO.Path]::GetFullPath($Root))
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing a package link: $($item.FullName)" }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            else { $item.FullName }
        }
    }
}

# Publish a staged file on the same filesystem without truncating its live copy.
function Move-VentureInstallFile([string]$Temporary, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination -PathType Leaf) { [IO.File]::Replace($Temporary, $Destination, [NullString]::Value) }
    else { [IO.File]::Move($Temporary, $Destination) }
}

function Write-VentureInstallState([string]$Path, [object]$Value) {
    $temporary = Join-Path ([IO.Path]::GetDirectoryName($Path)) ('venture-copy-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temporary, (($Value | ConvertTo-Json -Depth 6) + "`n"), [Text.UTF8Encoding]::new($false))
        Move-VentureInstallFile $temporary $Path
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Copy-VentureInstallFile([string]$Source, [string]$Destination, [long]$Bytes, [string]$Sha256) {
    # Stage one file at a time; no complete backup/version copy is retained.
    $temporary = Join-Path ([IO.Path]::GetDirectoryName($Destination)) ('venture-copy-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary -Force
        if ((Get-Item -LiteralPath $temporary).Length -ne $Bytes -or
            (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $Sha256) {
            throw "Installed file verification failed: $Destination"
        }
        Move-VentureInstallFile $temporary $Destination
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Install-VentureLayout([string]$Layout, [string]$InstallDir, [string]$Revision) {
    $Layout = [IO.Path]::GetFullPath($Layout).TrimEnd([char[]]'\/')
    $InstallDir = [IO.Path]::GetFullPath($InstallDir).TrimEnd([char[]]'\/')
    if ($InstallDir -eq [IO.Path]::GetPathRoot($InstallDir).TrimEnd([char[]]'\/') -or
        ($InstallDir + [IO.Path]::DirectorySeparatorChar).StartsWith($Layout + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        ($Layout + [IO.Path]::DirectorySeparatorChar).StartsWith($InstallDir + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Choose an installation directory separate from the generated package.'
    }
    Assert-NoVentureReparsePoint $Layout
    Assert-NoVentureReparsePoint $InstallDir
    $files = Get-VentureManifest $Layout
    $prefix = $Layout + [IO.Path]::DirectorySeparatorChar
    foreach ($path in Get-VentureTreeFiles $Layout) {
        $name = $path.Substring($prefix.Length).Replace('\', '/')
        if ($name -ne 'release-files.json' -and -not $files.ContainsKey($name)) { throw "Untracked package file: $name" }
    }
    foreach ($name in $files.Keys) {
        $path = Get-VentureManagedPath $Layout $name
        if ((Get-Item -LiteralPath $path).Length -ne $files[$name].Bytes -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $files[$name].Sha256) { throw "Package verification failed: $name" }
        $null = Get-VentureManagedPath $InstallDir $name
    }
    $previous = $null
    $manifestPath = Get-VentureManagedPath $InstallDir 'release-files.json'
    $statePath = Get-VentureManagedPath $InstallDir '.venture-local-install.json'
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) { $previous = Get-VentureManifest $InstallDir }
    elseif ((Test-Path -LiteralPath $InstallDir) -and @(Get-ChildItem -LiteralPath $InstallDir -Force).Count) {
        if (-not (Test-Path -LiteralPath $statePath -PathType Leaf) -or
            (Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json).owner -ne 'venture-local-build') {
            throw 'The destination is not a release layout or an owned local installation.'
        }
    }
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Write-VentureInstallState $statePath @{ owner = 'venture-local-build'; status = 'updating'; revision = $Revision }
    foreach ($name in $files.Keys) {
        $destination = Get-VentureManagedPath $InstallDir $name
        New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($destination)) | Out-Null
        Copy-VentureInstallFile (Get-VentureManagedPath $Layout $name) $destination $files[$name].Bytes $files[$name].Sha256
    }
    if ($null -ne $previous) {
        foreach ($name in $previous.Keys) {
            if (-not $files.ContainsKey($name)) {
                $obsolete = Get-VentureManagedPath $InstallDir $name
                if (Test-Path -LiteralPath $obsolete -PathType Leaf) { Remove-Item -LiteralPath $obsolete -Force }
            }
        }
    }
    $sourceManifest = Get-VentureManagedPath $Layout 'release-files.json'
    Copy-VentureInstallFile $sourceManifest $manifestPath (Get-Item -LiteralPath $sourceManifest).Length (Get-FileHash -LiteralPath $sourceManifest -Algorithm SHA256).Hash
    Write-VentureInstallState $statePath @{ owner = 'venture-local-build'; status = 'ready'; revision = $Revision }
    return (Join-Path $InstallDir 'venture.exe')
}

function Remove-VentureBuildPackage([string]$Path, [string]$Parent) {
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Path)) -ne [IO.Path]::GetFullPath($Parent) -or
        [IO.Path]::GetFileName($Path) -notmatch '^package-[a-f0-9]{32}$') { throw 'Not an owned build package.' }
    if (Test-Path -LiteralPath $Path) {
        $null = @(Get-VentureTreeFiles $Path)
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Get-VentureCMakeValue([string]$Cache, [string]$Name) {
    $pattern = '^' + [Regex]::Escape($Name) + ':[^=]+=(.*)$'
    foreach ($line in Get-Content -LiteralPath $Cache) {
        if ($line -match $pattern) { return $Matches[1] }
    }
    throw "Missing CMake configuration value: $Name"
}

# ProgramW6432 also selects the native x64 directory from a 32-bit shell.
function Get-VentureInstallConfiguration([string]$SourceRoot, [string]$InstallDir) {
    $directoryName = 'Venture'
    $shortcutName = 'Venture'
    $profile = Join-Path $SourceRoot '.venture-install.json'
    if (Test-Path -LiteralPath $profile -PathType Leaf) {
        $configuration = Get-Content -LiteralPath $profile -Raw | ConvertFrom-Json
        $directoryName = [string]$configuration.installDirectoryName
        $shortcutName = [string]$configuration.shortcutName
        if ($directoryName -notmatch '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$' -or
            $shortcutName -notmatch '^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$') { throw 'Invalid local-install profile names.' }
    }
    if ([string]::IsNullOrWhiteSpace($InstallDir)) {
        $programFiles = [Environment]::GetEnvironmentVariable('ProgramW6432')
        if (-not $programFiles) { $programFiles = [Environment]::GetFolderPath('ProgramFiles') }
        if (-not $programFiles) { throw 'The Windows Program Files directory could not be found.' }
        $InstallDir = Join-Path $programFiles $directoryName
    }
    return [pscustomobject]@{ InstallDir = [IO.Path]::GetFullPath($InstallDir).TrimEnd([char[]]'\/'); ShortcutName = $shortcutName }
}

function Test-VentureInstallWritable([string]$InstallDir) {
    Assert-NoVentureReparsePoint $InstallDir
    $parent = [IO.Path]::GetFullPath($InstallDir)
    while (-not [IO.Directory]::Exists($parent)) {
        $parent = [IO.Path]::GetDirectoryName($parent)
        if (-not $parent) { throw 'The installation drive does not exist.' }
    }
    $temporary = Join-Path $parent ('venture-access-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $probe = $null
    try {
        $probe = [IO.FileStream]::new($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
            [IO.FileShare]::None, 1, [IO.FileOptions]::DeleteOnClose)
        return $true
    } catch [UnauthorizedAccessException] { return $false }
    catch [Security.SecurityException] { return $false }
    finally { if ($null -ne $probe) { $probe.Dispose() } }
}

# Start-Process joins ArgumentList with spaces. Quote literal -File arguments;
# never interpret a checkout path as a PowerShell command or expression.
function Join-VentureProcessArguments([string[]]$Arguments) {
    return (($Arguments | ForEach-Object {
        if ($_ -match '["\r\n\x00]') { throw 'Invalid installer process argument.' }
        '"' + [Regex]::Replace($_, '(\\+)$', '$1$1') + '"'
    }) -join ' ')
}

function Install-VenturePackage([string]$Layout, [string]$InstallDir, [string]$Revision,
                                [string]$WorkerScript, [string]$LogPath) {
    if (Test-VentureInstallWritable $InstallDir) { return Install-VentureLayout $Layout $InstallDir $Revision }
    Write-Host "Windows administrator approval is required to install into $InstallDir."
    $windowsShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not [Environment]::Is64BitProcess) {
        $windowsShell = Join-Path $env:SystemRoot 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
    }
    $arguments = Join-VentureProcessArguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        $WorkerScript, '-Layout', $Layout, '-InstallDir', $InstallDir, '-Revision', $Revision, '-LogPath', $LogPath)
    try {
        $child = Start-Process -FilePath $windowsShell -Verb RunAs -ArgumentList $arguments -Wait -PassThru -ErrorAction Stop
    } catch { throw "Administrator installation could not start or was cancelled: $($_.Exception.Message)" }
    finally {
        if (Test-Path -LiteralPath $LogPath -PathType Leaf) {
            Get-Content -LiteralPath $LogPath | ForEach-Object { Write-Host $_ }
        }
    }
    if ($child.ExitCode -ne 0) { throw "Administrator installation failed (exit $($child.ExitCode)). See $LogPath" }
    $receipt = Get-Content -LiteralPath (Get-VentureManagedPath $InstallDir '.venture-local-install.json') -Raw | ConvertFrom-Json
    if ($receipt.owner -ne 'venture-local-build' -or $receipt.status -ne 'ready' -or $receipt.revision -ne $Revision) {
        throw 'The administrator installation did not publish the expected successful receipt.'
    }
    return (Get-VentureManagedPath $InstallDir 'venture.exe')
}
