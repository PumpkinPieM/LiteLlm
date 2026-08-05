param(
  [ValidateSet('host', 'arm64-v8a', 'x86_64')]
  [string]$Arch = 'host',
  [ValidateSet('Debug', 'Release')]
  [string]$BuildType = 'Release',
  [string]$SdkRoot = '',
  [string]$BuildDir = '',
  [switch]$RealRuntime,
  [switch]$WithoutTests
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$mockValue = if ($RealRuntime) { 'OFF' } else { 'ON' }
$testsValue = if ($WithoutTests) { 'OFF' } else { 'ON' }

if ($BuildDir.Length -eq 0) {
  $BuildDir = Join-Path $projectRoot ("build-$Arch")
}

if ($Arch -eq 'host') {
  $cmakeCommand = Get-Command cmake -ErrorAction Stop
  & $cmakeCommand.Source -S $projectRoot -B $BuildDir `
    "-DCMAKE_BUILD_TYPE=$BuildType" `
    "-DLITE_LLM_USE_MOCK_RUNTIME=$mockValue" `
    '-DLITE_LLM_BUILD_SERVER=ON' `
    "-DLITE_LLM_BUILD_TESTS=$testsValue"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  & $cmakeCommand.Source --build $BuildDir --config $BuildType
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
  if ($env:OHOS_NDK) {
    $ohosNative = $env:OHOS_NDK
    $hmsNative = $env:OHOS_NDK
  } else {
    if ($SdkRoot.Length -eq 0) {
      if ($env:DEVECO_SDK_HOME -and (Test-Path -LiteralPath $env:DEVECO_SDK_HOME)) {
        $SdkRoot = $env:DEVECO_SDK_HOME
      } else {
        $homeMarker = Join-Path $env:LOCALAPPDATA 'Huawei\DevEcoStudio6.1\.home'
        if (Test-Path -LiteralPath $homeMarker) {
          $studioHome = (Get-Content -Raw -LiteralPath $homeMarker).Trim()
          $SdkRoot = Join-Path $studioHome 'sdk'
        }
      }
    }
    if ($SdkRoot.Length -eq 0 -or !(Test-Path -LiteralPath $SdkRoot)) {
      throw 'Unable to locate the DevEco SDK. Pass -SdkRoot, set DEVECO_SDK_HOME, or set OHOS_NDK.'
    }

    $resolvedSdk = (Resolve-Path -LiteralPath $SdkRoot).Path
    $ohosNative = Join-Path $resolvedSdk 'default\openharmony\native'
    $hmsNative = Join-Path $resolvedSdk 'default\hms\native'
  }
  $cmake = Join-Path $ohosNative 'build-tools\cmake\bin\cmake.exe'
  $ninja = Join-Path $ohosNative 'build-tools\cmake\bin\ninja.exe'
  $toolchain = Join-Path $hmsNative 'build\cmake\hmos.toolchain.bisheng.cmake'
  foreach ($required in @($cmake, $ninja, $toolchain)) {
    if (!(Test-Path -LiteralPath $required)) {
      throw "Required SDK tool not found: $required"
    }
  }

  & $cmake -S $projectRoot -B $BuildDir -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DHMOS_SDK_NATIVE=$hmsNative" `
    "-DOHOS_SDK_NATIVE=$ohosNative" `
    '-DCMAKE_SYSTEM_NAME=OHOS' `
    "-DOHOS_ARCH=$Arch" `
    "-DCMAKE_OHOS_ARCH_ABI=$Arch" `
    "-DCMAKE_BUILD_TYPE=$BuildType" `
    "-DLITE_LLM_USE_MOCK_RUNTIME=$mockValue" `
    '-DLITE_LLM_BUILD_SERVER=ON' `
    "-DLITE_LLM_BUILD_TESTS=$testsValue"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  & $cmake --build $BuildDir
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Output "Built liblite_llm.so and lite-server in $BuildDir"
