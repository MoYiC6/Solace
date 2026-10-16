[CmdletBinding()]
param(
    [string]$OriginalDll = (Join-Path $PSScriptRoot '..\Solace.dll'),
    [Parameter(Mandatory = $true)][string]$ClientDll,
    [string]$Profile = (Join-Path $PSScriptRoot 'profiles\solace-build14191.json'),
    [string]$Output = (Join-Path $PSScriptRoot 'dist\Solace.esp-bootstrap.dll'),
    [string]$Python = 'python',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$taskVenv = Join-Path $PSScriptRoot '.venv'
$taskPython = Join-Path $taskVenv 'Scripts\python.exe'
if (-not (Test-Path -LiteralPath $taskPython)) {
    & $Python -m venv $taskVenv
    if ($LASTEXITCODE -ne 0) { throw 'Creating the local Python environment failed.' }
}
& $taskPython -m pip --isolated install --disable-pip-version-check --index-url https://pypi.org/simple --only-binary=:all: --require-hashes -r (Join-Path $PSScriptRoot 'requirements.lock')
if ($LASTEXITCODE -ne 0) { throw 'Installing the pinned Python dependencies failed.' }
& $taskPython (Join-Path $PSScriptRoot 'src\build.py') --input $OriginalDll --client $ClientDll --profile $Profile --output $Output
if ($LASTEXITCODE -ne 0) { throw 'Building the candidate failed.' }
$taskManifest = [IO.Path]::ChangeExtension([IO.Path]::GetFullPath($Output), '.manifest.json')
if (-not $SkipTests) {
    $taskResults = [IO.Path]::ChangeExtension([IO.Path]::GetFullPath($Output), '.validation.json')
    & $taskPython (Join-Path $PSScriptRoot 'tests\validate_bootstrap.py') --dll $Output --manifest $taskManifest --output $taskResults
    if ($LASTEXITCODE -ne 0) { throw 'Independent machine-code validation failed.' }
}
Write-Output "Built: $([IO.Path]::GetFullPath($Output))"
Write-Output 'The candidate has not been tested in the game by this build command.'
