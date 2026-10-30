param([string]$PythonPath = 'python.exe')
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\tools\windows\build_helpers.ps1')
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('venture-build-log-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$log = Join-Path $fixtureRoot 'transcript.log'
$transcribing = $false
try {
    $failureFixture = Join-Path $fixtureRoot 'failing child with spaces.py'
    @'
import sys
print("stdout before failure", flush=True)
sys.stderr.write("Traceback (most recent call last):\n")
sys.stderr.write("  File fixture.py, line 1\nRuntimeError: verification fixture\n")
sys.stderr.flush()
print("stdout after stderr", flush=True)
if sys.argv[1:] != ["argument with spaces"]:
    sys.exit(9)
sys.exit(7)
'@ | Set-Content -LiteralPath $failureFixture -Encoding UTF8
    $successFixture = Join-Path $fixtureRoot 'successful child.py'
    @'
import sys
print("success stdout", flush=True)
sys.stderr.write("success stderr\n")
'@ | Set-Content -LiteralPath $successFixture -Encoding UTF8
    Start-Transcript -Path $log | Out-Null
    $transcribing = $true
    $failed = $false
    try {
        Invoke-BuildStep 'Expected failing fixture' $PythonPath @($failureFixture, 'argument with spaces')
    } catch {
        if ($_.Exception.Message -notlike '*exit 7*') { throw }
        $failed = $true
    }
    if (-not $failed) { throw 'A failing child must fail the build step.' }
    if ($ErrorActionPreference -ne 'Stop') { throw 'The caller error preference changed.' }
    Invoke-BuildStep 'Successful fixture with stderr' $PythonPath @($successFixture)
    $missingRejected = $false
    try {
        Invoke-BuildStep 'Missing executable' (Join-Path $fixtureRoot 'missing.exe') @()
    } catch { $missingRejected = $true }
    if (-not $missingRejected) { throw 'A missing executable must fail the build step.' }
    Stop-Transcript | Out-Null
    $transcribing = $false
    $text = Get-Content -LiteralPath $log -Raw
    foreach ($expected in @('stdout before failure', 'stdout after stderr',
        'Traceback (most recent call last):', 'RuntimeError: verification fixture',
        'success stdout', 'success stderr')) {
        if (-not $text.Contains($expected)) { throw "Transcript is missing: $expected" }
    }
    Write-Host 'PASS native stdout/stderr, transcript tracebacks and exit codes'
} finally {
    if ($transcribing) { Stop-Transcript | Out-Null }
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}
