function Assert-VentureStopped {
    $running = @(Get-Process -Name 'venture', 'venture-runtime' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw 'Close Venture and its running game, then rerun the installer. The passthrough files are updated in place.'
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
