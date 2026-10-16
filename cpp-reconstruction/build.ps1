[CmdletBinding()]
param(
    [string]$CMake,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [string]$BuildDirectory
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 -File does not reliably initialize PSScriptRoot while
# evaluating parameter defaults. Resolve the default after parameter binding.
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot 'build' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer vswhere.exe is required.' }
$installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)[0]
if (-not $installation) { throw 'Visual Studio with the Desktop C++ toolchain is required.' }
$major = ([version]$installation.installationVersion).Major
switch ($major) {
    18 { $generator = 'Visual Studio 18 2026' }
    17 { $generator = 'Visual Studio 17 2022' }
    default { throw "Unsupported Visual Studio version: $major" }
}
if (-not $CMake) {
    $CMake = Join-Path $installation.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $CMake)) { throw 'CMake was not found; specify its path with -CMake.' }
& $CMake -S $PSScriptRoot -B $BuildDirectory -G $generator -A x64 -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed: $LASTEXITCODE" }
& $CMake --build $BuildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "C++ compilation failed: $LASTEXITCODE" }
$ctest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
& $ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure --test-output-size-passed 16384 --output-junit (Join-Path $BuildDirectory 'test-results.xml')
if ($LASTEXITCODE -ne 0) { throw "C++ tests failed: $LASTEXITCODE" }
Write-Host "Built and tested $BuildDirectory\$Configuration\solace-tool.exe"
