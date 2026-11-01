Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\tools\windows\build_helpers.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('venture-build-install-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
function Check([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
function Refuses([scriptblock]$Action) {
    $failed = $false
    try { $null = & $Action } catch { $failed = $true }
    Check $failed 'An unsafe or invalid installation was accepted.'
}
function Layout([string]$Name, [string]$Value, [bool]$Old = $false) {
    $root = Join-Path $fixture $Name
    New-Item -ItemType Directory -Path $root | Out-Null
    $files = [ordered]@{}
    $names = @('venture.exe', 'venture-runtime.exe', 'licenses/notice.txt')
    if ($Old) { $names += 'obsolete.bin' }
    foreach ($name in $names) {
        $path = Join-Path $root $name
        New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($path)) | Out-Null
        [IO.File]::WriteAllText($path, $Value + ':' + $name)
        $files[$name] = @{ bytes = (Get-Item -LiteralPath $path).Length; sha256 = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() }
    }
    Write-VentureInstallState (Join-Path $root 'release-files.json') @{files = $files}
    return $root
}
try {
    $old = Layout 'old' 'before' $true
    $new = Layout 'new' 'after'
    $destination = Join-Path $fixture 'installed'
    $null = Install-VentureLayout $old $destination 'revision-a'
    [IO.File]::WriteAllText((Join-Path $destination 'my-notes.txt'), 'keep user files')
    $starter = Install-VentureLayout $new $destination 'revision-b'
    Check ($starter -eq (Join-Path $destination 'venture.exe')) 'Wrong launcher path.'
    Check ((Get-Content -LiteralPath $starter -Raw) -eq 'after:venture.exe') 'Update did not replace the executable.'
    Check (-not (Test-Path (Join-Path $destination 'obsolete.bin'))) 'Obsolete managed file survived.'
    Check ((Get-Content -LiteralPath (Join-Path $destination 'my-notes.txt') -Raw) -eq 'keep user files') 'User file changed.'
    $state = Get-Content -LiteralPath (Join-Path $destination '.venture-local-install.json') -Raw | ConvertFrom-Json
    Check ($state.status -eq 'ready' -and $state.revision -eq 'revision-b') 'Wrong successful install receipt.'
    Check (-not (Test-Path (Join-Path $destination 'versions'))) 'An unnecessary versions directory was created.'
    $partial = Layout 'partial' 'replacement'
    function Copy-Item {
        param([string]$LiteralPath, [string]$Destination, [switch]$Force)
        if ([IO.Path]::GetFileName($LiteralPath) -eq 'venture.exe') {
            [IO.File]::WriteAllText($Destination, 'interrupted write')
            throw 'Injected copy failure after a partial write'
        }
        Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
    }
    try { Refuses { Install-VentureLayout $partial $destination 'failed-copy' } }
    finally { Remove-Item Function:Copy-Item }
    Check ((Get-Content -LiteralPath $starter -Raw) -eq 'after:venture.exe') 'Failed copy truncated the installed executable.'
    Check (@(Get-ChildItem -LiteralPath $destination -Filter 'venture-copy-*' -Recurse -Force).Count -eq 0) 'Failed copy leaked a temporary file.'
    $null = Install-VentureLayout $new $destination 'revision-b'
    $bad = Layout 'bad' 'tampered'
    Add-Content -LiteralPath (Join-Path $bad 'venture.exe') -Value 'changed after manifest'
    Refuses { Install-VentureLayout $bad $destination 'bad-hash' }
    Check ((Get-Content -LiteralPath $starter -Raw) -eq 'after:venture.exe') 'Source verification happened after modifying the install.'
    $extra = Layout 'extra' 'extra'
    [IO.File]::WriteAllText((Join-Path $extra 'untracked.bin'), 'not packaged')
    Refuses { Install-VentureLayout $extra $destination 'untracked' }
    $traversal = Layout 'traversal' 'invalid'
    $manifest = Get-Content -LiteralPath (Join-Path $traversal 'release-files.json') -Raw | ConvertFrom-Json
    $manifest.files | Add-Member -NotePropertyName '../outside.bin' -NotePropertyValue @{bytes = 0; sha256 = ('0' * 64)}
    Write-VentureInstallState (Join-Path $traversal 'release-files.json') $manifest
    Refuses { Install-VentureLayout $traversal $destination 'traversal' }
    Refuses { Install-VentureLayout $new $new 'overlap' }
    $unowned = Join-Path $fixture 'unowned'
    New-Item -ItemType Directory -Path $unowned | Out-Null
    [IO.File]::WriteAllText((Join-Path $unowned 'notes'), 'not an installation')
    Refuses { Install-VentureLayout $new $unowned 'unowned' }
    $repair = Join-Path $fixture 'repair'
    New-Item -ItemType Directory -Path (Join-Path $repair 'venture-runtime.exe') -Force | Out-Null
    Write-VentureInstallState (Join-Path $repair '.venture-local-install.json') @{owner = 'venture-local-build'; status = 'updating'}
    Refuses { Install-VentureLayout $new $repair 'interrupted' }
    Check ((Get-Content -LiteralPath (Join-Path $repair '.venture-local-install.json') -Raw | ConvertFrom-Json).status -eq 'updating') 'Partial copy was reported ready.'
    Remove-Item -LiteralPath (Join-Path $repair 'venture-runtime.exe') -Recurse -Force
    $null = Install-VentureLayout $new $repair 'repaired'
    Check ((Get-Content -LiteralPath (Join-Path $repair '.venture-local-install.json') -Raw | ConvertFrom-Json).status -eq 'ready') 'Rerun did not repair an interrupted install.'
    $outside = Join-Path $fixture 'outside'
    New-Item -ItemType Directory -Path $outside | Out-Null
    [IO.File]::WriteAllText((Join-Path $outside 'notice.txt'), 'do not touch')
    Remove-Item -LiteralPath (Join-Path $destination 'licenses') -Recurse -Force
    $linkType = if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) { 'Junction' } else { 'SymbolicLink' }
    New-Item -ItemType $linkType -Path (Join-Path $destination 'licenses') -Target $outside | Out-Null
    Refuses { Install-VentureLayout $new $destination 'redirected' }
    Check ((Get-Content -LiteralPath (Join-Path $outside 'notice.txt') -Raw) -eq 'do not touch') 'Install followed a destination link.'
    $packages = Join-Path $fixture 'packages'
    $package = Join-Path $packages ('package-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $package -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $package 'partial'), 'incomplete package')
    Remove-VentureBuildPackage $package $packages
    Check (-not (Test-Path $package)) 'Owned partial package was retained.'
    Refuses { Remove-VentureBuildPackage $outside $fixture }
    $cache = Join-Path $fixture 'CMakeCache.txt'
    [IO.File]::WriteAllText($cache, 'VENTURE_ANGLE_ROOT:PATH=C:/path with spaces/angle')
    Check ((Get-VentureCMakeValue $cache 'VENTURE_ANGLE_ROOT') -eq 'C:/path with spaces/angle') 'CMake path with spaces was changed.'
    Write-Host 'PASS verified in-place install, stale cleanup, user-file retention, failed-copy recovery, traversal/junction refusal and owned package cleanup'
} finally {
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
