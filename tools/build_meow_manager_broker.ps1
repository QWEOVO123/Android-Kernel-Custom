param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$ManagerCertificateSHA256,
    [string]$JavaHome = $env:JAVA_HOME,
    [string]$Sdk = $env:ANDROID_HOME,
    [string]$BuildTools = '37.0.0'
)
$ErrorActionPreference = 'Stop'
if (!$JavaHome -or !$Sdk) { throw 'Set JAVA_HOME and ANDROID_HOME or pass -JavaHome and -Sdk.' }
$workspace = Split-Path -Parent $PSScriptRoot
$android = Join-Path $Sdk 'platforms/android-34/android.jar'
$d8 = Join-Path $Sdk "build-tools/$BuildTools/lib/d8.jar"
$wire = @(Get-ChildItem (Join-Path $workspace 'QweovoManager/manager-protocol/src/main/java/io/qweovo/auth') -Filter '*.java' | ForEach-Object FullName)
$broker = @(Get-ChildItem (Join-Path $PSScriptRoot 'meow-manager-broker/io/qweovo/auth') -Filter '*.java' | ForEach-Object FullName)
$out = Join-Path $workspace 'out/manager-broker'
$stage = Join-Path ([IO.Path]::GetTempPath()) "meow-broker-$([guid]::NewGuid())"
$classes = Join-Path $stage 'classes'
New-Item -ItemType Directory -Force -Path $out,$classes | Out-Null
try {
    $digestBytes = for ($i = 0; $i -lt 64; $i += 2) {
        '(byte)0x' + $ManagerCertificateSHA256.Substring($i, 2)
    }
    $identityPath = Join-Path $stage 'ManagerCertificate.java'
    $identity = 'package io.qweovo.auth; final class ManagerCertificate { static final byte[] SHA256 = {' +
        ($digestBytes -join ', ') + '}; }'
    Set-Content -LiteralPath $identityPath -Value $identity -Encoding ascii
    & (Join-Path $JavaHome 'bin/javac') -encoding UTF-8 --release 17 -cp $android -d $classes @wire @broker $identityPath
    if ($LASTEXITCODE -ne 0) { throw 'Broker Java compilation failed' }
    $bytecode = @(Get-ChildItem -LiteralPath $classes -Recurse -Filter '*.class' | ForEach-Object FullName)
    $jar = Join-Path $stage 'service.jar'
    & (Join-Path $JavaHome 'bin/java') -cp $d8 com.android.tools.r8.D8 --release --min-api 31 --lib $android --output $jar @bytecode
    if ($LASTEXITCODE -ne 0) { throw 'Broker DEX packaging failed' }
    $output = Join-Path $out 'service.jar'
    Copy-Item -LiteralPath $jar -Destination $output -Force
    Get-Item -LiteralPath $output
} finally {
    $resolvedTemp = (Resolve-Path -LiteralPath ([IO.Path]::GetTempPath())).Path.TrimEnd([IO.Path]::DirectorySeparatorChar)
    $resolvedStage = (Resolve-Path -LiteralPath $stage).Path
    if ((Split-Path -Parent $resolvedStage) -eq $resolvedTemp) {
        Remove-Item -LiteralPath $resolvedStage -Recurse
    }
}