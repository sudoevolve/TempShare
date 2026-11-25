param([string]$BuildDirectory = 'build', [string]$Compiler)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$staging = Join-Path $build ('package-staging-' + [Guid]::NewGuid().ToString('N'))
$output = Join-Path $build 'package'
$match = Select-String -LiteralPath (Join-Path $root 'CMakeLists.txt') -Pattern '^project\(TempShare VERSION ([0-9.]+) '
$version = $match.Matches[0].Groups[1].Value
if (!$Compiler) {
    $Compiler = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
    if (!$Compiler) {
        $Compiler = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Inno Setup 6/ISCC.exe'
    }
}
if (!(Test-Path -LiteralPath $Compiler -PathType Leaf)) { throw 'Specify Inno Setup 6 ISCC.exe with -Compiler' }
& cmake --install $build --config Release --prefix $staging
if ($LASTEXITCODE) { throw 'CMake install failed' }
& $Compiler "/DAppVersion=$version" "/DStagingDir=$staging" "/DOutputPath=$output" (Join-Path $PSScriptRoot 'TempShare.iss')
if ($LASTEXITCODE) { throw 'Inno Setup build failed' }
$installer = Join-Path $output "TempShare-$version-windows-x64.exe"
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($installer))" | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ASCII
Get-Item -LiteralPath $installer
