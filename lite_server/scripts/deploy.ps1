param(
  [ValidateSet('arm64-v8a', 'x86_64')]
  [string]$Arch = 'x86_64',
  [string]$BuildDir = '',
  [string]$SdkRoot = '',
  [string]$HdcPath = '',
  [string]$RemoteDir = '/data/local/tmp/lite-server',
  [string]$ConfigPath = '',
  [int]$ReversePort = 18081,
  [Parameter(Mandatory = $true)]
  [string]$AuthToken,
  [switch]$RunTests
)

$ErrorActionPreference = 'Stop'

function Resolve-SdkRoot([string]$Requested) {
  if ($Requested.Length -gt 0) {
    return (Resolve-Path -LiteralPath $Requested).Path
  }
  if ($env:DEVECO_SDK_HOME -and (Test-Path -LiteralPath $env:DEVECO_SDK_HOME)) {
    return (Resolve-Path -LiteralPath $env:DEVECO_SDK_HOME).Path
  }
  $homeMarker = Join-Path $env:LOCALAPPDATA 'Huawei\DevEcoStudio6.1\.home'
  if (Test-Path -LiteralPath $homeMarker) {
    $studioHome = (Get-Content -Raw -LiteralPath $homeMarker).Trim()
    $candidate = Join-Path $studioHome 'sdk'
    if (Test-Path -LiteralPath $candidate) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }
  throw 'Unable to locate the DevEco SDK. Pass -SdkRoot or set DEVECO_SDK_HOME.'
}

if ($RemoteDir -notmatch '^/data/local/tmp/[A-Za-z0-9._/-]+$') {
  throw 'RemoteDir must be a path below /data/local/tmp containing only safe path characters.'
}
if ($AuthToken -notmatch '^[A-Za-z0-9._-]{16,128}$') {
  throw 'AuthToken must contain 16-128 letters, digits, dots, underscores, or hyphens.'
}
if ($ReversePort -lt 1 -or $ReversePort -gt 65535) {
  throw 'ReversePort must be between 1 and 65535.'
}

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$resolvedSdk = Resolve-SdkRoot $SdkRoot
if ($BuildDir.Length -eq 0) {
  $BuildDir = Join-Path $projectRoot ("build-$Arch")
}
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
if ($ConfigPath.Length -eq 0) {
  $ConfigPath = Join-Path $projectRoot 'lite_server\examples\mock\config.json'
}
$ConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path

if ($HdcPath.Length -eq 0) {
  $HdcPath = Join-Path $resolvedSdk 'default\openharmony\toolchains\hdc.exe'
}
$HdcPath = (Resolve-Path -LiteralPath $HdcPath).Path
$server = Join-Path $BuildDir 'bin\lite-server'
$tests = Join-Path $BuildDir 'bin\lite-server-tests'
$library = Join-Path $BuildDir 'lib\liblite_llm.so'
$runtime = Join-Path $BuildDir 'lib\liblite_llm_runtime.so'
$runtimeTriple = if ($Arch -eq 'arm64-v8a') { 'aarch64-linux-ohos' } else { 'x86_64-linux-ohos' }
$libcxx = Join-Path $resolvedSdk ("default\hms\native\BiSheng\lib\$runtimeTriple\libc++_shared.so")
foreach ($required in @($server, $library, $runtime, $libcxx, $ConfigPath)) {
  if (!(Test-Path -LiteralPath $required)) {
    throw "Required deployment file not found: $required"
  }
}

& $HdcPath shell "mkdir -p $RemoteDir"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
foreach ($file in @($server, $library, $runtime, $libcxx)) {
  & $HdcPath file send $file "$RemoteDir/"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& $HdcPath file send $ConfigPath "$RemoteDir/config.json"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $HdcPath shell "chmod 700 $RemoteDir/lite-server"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($RunTests) {
  if (!(Test-Path -LiteralPath $tests)) {
    throw "Test executable not found: $tests"
  }
  & $HdcPath file send $tests "$RemoteDir/"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  & $HdcPath shell "chmod 700 $RemoteDir/lite-server-tests"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  & $HdcPath shell "cd $RemoteDir && LD_LIBRARY_PATH=. ./lite-server-tests ./config.json"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Output 'Deployment complete. Launch after the app proxy is listening:'
Write-Output "hdc shell `"cd $RemoteDir && LD_LIBRARY_PATH=. ./lite-server --config ./config.json --connect-host 127.0.0.1 --connect-port $ReversePort --auth-token $AuthToken`""
