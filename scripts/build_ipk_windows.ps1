[CmdletBinding()]
param(
    [string]$WorkDir = '',
    [string]$SdkRoot = '',
    [string]$ToolchainFile = '',
    [string]$AresPackage = '',
    [string]$CMake = '',
    [string]$CPack = '',
    [string]$Ninja = '',
    [string]$WebOsSdkInstaller = '',
    [switch]$NoInstallPrereqs
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SourceDir = Split-Path -Parent $ScriptDir
$WorkspaceDir = Split-Path -Parent $SourceDir
$RootDistDir = Join-Path $WorkspaceDir 'dist'
if (-not $WorkDir) {
    $WorkDir = Join-Path $env:TEMP 'ctm-bridge-test-webos-build'
}

function Find-FirstExisting([string[]]$Paths) {
    foreach ($path in $Paths) {
        if ($path -and (Test-Path -LiteralPath $path)) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }
    return $null
}

function Find-CommandPath([string]$Name, [string[]]$ExtraCandidates = @()) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return Find-FirstExisting $ExtraCandidates
}

function Install-ChocoPackage([string]$PackageName, [string]$DisplayName) {
    if ($NoInstallPrereqs) {
        throw "$DisplayName not found and -NoInstallPrereqs was supplied."
    }
    $choco = Find-CommandPath 'choco.exe'
    if (-not $choco) {
        throw "$DisplayName not found and Chocolatey is not installed. Install it manually or pass the tool path."
    }
    Write-Host "Installing missing prerequisite with Chocolatey: $PackageName"
    & $choco install $PackageName -y --no-progress
    if ($LASTEXITCODE -ne 0) {
        throw "Chocolatey failed to install $PackageName."
    }
}

function Invoke-WebOsSdkInstaller([string]$InstallerPath) {
    if ($NoInstallPrereqs) {
        throw 'webOS CLI/SDK/NDK missing and -NoInstallPrereqs was supplied.'
    }
    if (-not $InstallerPath) {
        return
    }
    if (-not (Test-Path -LiteralPath $InstallerPath)) {
        throw "webOS SDK installer not found: $InstallerPath"
    }
    Write-Host "Running webOS SDK installer: $InstallerPath"
    $proc = Start-Process -FilePath $InstallerPath -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        throw "webOS SDK installer exited with code $($proc.ExitCode)."
    }
}

function Find-AresPackagePath {
    $path = Find-CommandPath 'ares-package.cmd' @(
        (Join-Path $WorkspaceDir 'beanviser\ares-cli\bin\ares-package.cmd'),
        'C:\webOS_TV_SDK\CLI\bin\ares-package.cmd',
        'C:\webOS_TV_SDK\CLI\bin\ares-package.bat',
        'C:\Program Files\LG Electronics\webOS TV SDK\CLI\bin\ares-package.cmd',
        'C:\Program Files (x86)\LG Electronics\webOS TV SDK\CLI\bin\ares-package.cmd'
    )
    if ($path) { return $path }
    return Find-CommandPath 'ares-package.exe'
}

function Find-WebOsSdkRoot {
    if ($env:WEBOS_SDK_ROOT) { return $env:WEBOS_SDK_ROOT }
    if ($env:WEBOS_NDK_ROOT) { return $env:WEBOS_NDK_ROOT }
    return Find-FirstExisting @(
        'C:\webos-sdk\arm-webos-linux-gnueabi_sdk-buildroot',
        'C:\webOS_TV_SDK\Resources\NDK\arm-webos-linux-gnueabi_sdk-buildroot',
        'C:\Program Files\LG Electronics\webOS TV SDK\Resources\NDK\arm-webos-linux-gnueabi_sdk-buildroot',
        'C:\Program Files (x86)\LG Electronics\webOS TV SDK\Resources\NDK\arm-webos-linux-gnueabi_sdk-buildroot'
    )
}

if (-not $CMake) {
    $CMake = Find-CommandPath 'cmake.exe' @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\CMake\bin\cmake.exe'
    )
}
if (-not $CMake) {
    Install-ChocoPackage 'cmake' 'cmake.exe'
    $CMake = Find-CommandPath 'cmake.exe' @('C:\Program Files\CMake\bin\cmake.exe')
}
if (-not $CMake) { throw 'cmake.exe not found after prerequisite install.' }

if (-not $CPack) {
    $cmakeDir = Split-Path -Parent $CMake
    $CPack = Find-CommandPath 'cpack.exe' @(
        (Join-Path $cmakeDir 'cpack.exe'),
        'C:\Program Files\CMake\bin\cpack.exe'
    )
}
if (-not $CPack) { throw 'cpack.exe not found beside CMake.' }

if (-not $Ninja) {
    $Ninja = Find-CommandPath 'ninja.exe' @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    )
}
if (-not $Ninja) {
    Install-ChocoPackage 'ninja' 'ninja.exe'
    $Ninja = Find-CommandPath 'ninja.exe'
}
if (-not $Ninja) { throw 'ninja.exe not found after prerequisite install.' }

if (-not $AresPackage) {
    $AresPackage = Find-AresPackagePath
}
if (-not $AresPackage) {
    Invoke-WebOsSdkInstaller $WebOsSdkInstaller
    $AresPackage = Find-AresPackagePath
}
if (-not $AresPackage) {
    throw 'ares-package not found. Install the webOS TV SDK CLI, add it to PATH, pass -AresPackage <path>, or pass -WebOsSdkInstaller <installer>.'
}

if (-not $SdkRoot) {
    $SdkRoot = Find-WebOsSdkRoot
}

if (-not $ToolchainFile) {
    if ($env:TOOLCHAIN_FILE) { $ToolchainFile = $env:TOOLCHAIN_FILE }
    elseif ($SdkRoot) { $ToolchainFile = Join-Path $SdkRoot 'share\buildroot\toolchainfile.cmake' }
}
if (-not $ToolchainFile -or -not (Test-Path -LiteralPath $ToolchainFile)) {
    Invoke-WebOsSdkInstaller $WebOsSdkInstaller
    if (-not $SdkRoot) { $SdkRoot = Find-WebOsSdkRoot }
    if (-not $ToolchainFile -and $SdkRoot) { $ToolchainFile = Join-Path $SdkRoot 'share\buildroot\toolchainfile.cmake' }
}
if (-not $ToolchainFile -or -not (Test-Path -LiteralPath $ToolchainFile)) {
    throw 'webOS native toolchainfile.cmake not found. Install the webOS Native SDK/NDK, pass -ToolchainFile <path>, or pass -WebOsSdkInstaller <installer>.'
}

Remove-Item -LiteralPath $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
New-Item -ItemType Directory -Force -Path $RootDistDir | Out-Null

$exclude = @('build', 'dist')
Get-ChildItem -LiteralPath $SourceDir -Force | Where-Object {
    $exclude -notcontains $_.Name
} | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $WorkDir -Recurse -Force
}

$iconPath = Join-Path $WorkDir 'icon.png'
if (-not (Test-Path -LiteralPath $iconPath)) {
    [byte[]]$png = [Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=')
    [IO.File]::WriteAllBytes($iconPath, $png)
}

$env:PATH = (Split-Path -Parent $AresPackage) + ';' +
            (Split-Path -Parent $CMake) + ';' +
            (Split-Path -Parent $Ninja) + ';' +
            $env:PATH

$buildDir = Join-Path $WorkDir 'build'
& $CMake -S $WorkDir -B $buildDir -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ToolchainFile"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

& $CMake --build $buildDir --target package
if ($LASTEXITCODE -ne 0) { throw 'CMake package build failed.' }

$ipks = Get-ChildItem -LiteralPath (Join-Path $WorkDir 'dist') -Filter '*.ipk' -File -ErrorAction SilentlyContinue
if (-not $ipks) { throw 'Build completed but no IPK was produced.' }

foreach ($ipk in $ipks) {
    Copy-Item -LiteralPath $ipk.FullName -Destination $RootDistDir -Force
}

Write-Host 'IPK output:'
Get-ChildItem -LiteralPath $RootDistDir -Filter 'com.local.ctmbridge_*.ipk' -File | Select-Object -ExpandProperty FullName
