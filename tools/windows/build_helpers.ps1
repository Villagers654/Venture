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

function Write-VentureInstallState([string]$Path, [object]$Value) {
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 6) + "`n"), [Text.UTF8Encoding]::new($false))
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
        # Stage one file in the same directory; failed copies cannot truncate
        # an existing executable. No complete backup/version copy is retained.
        $temporary = Join-Path ([IO.Path]::GetDirectoryName($destination)) ('venture-copy-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        try {
            Copy-Item -LiteralPath (Get-VentureManagedPath $Layout $name) -Destination $temporary -Force
            if ((Get-Item -LiteralPath $temporary).Length -ne $files[$name].Bytes -or
                (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $files[$name].Sha256) {
                throw "Installed file verification failed: $name"
            }
            if (Test-Path -LiteralPath $destination -PathType Leaf) { [IO.File]::Replace($temporary, $destination, [NullString]::Value) }
            else { [IO.File]::Move($temporary, $destination) }
        } finally {
            if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
        }
    }
    if ($null -ne $previous) {
        foreach ($name in $previous.Keys) {
            if (-not $files.ContainsKey($name)) {
                $obsolete = Get-VentureManagedPath $InstallDir $name
                if (Test-Path -LiteralPath $obsolete -PathType Leaf) { Remove-Item -LiteralPath $obsolete -Force }
            }
        }
    }
    Copy-Item -LiteralPath (Join-Path $Layout 'release-files.json') -Destination $manifestPath -Force
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
