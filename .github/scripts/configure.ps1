$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

if ($IsWindows) {
  $compilers = @(
    "-DCMAKE_C_COMPILER=$env:MINGW_ROOT/bin/gcc.exe",
    "-DCMAKE_CXX_COMPILER=$env:MINGW_ROOT/bin/g++.exe"
  )
} else {
  $compilers = @('-DCMAKE_C_COMPILER=clang-23', '-DCMAKE_CXX_COMPILER=clang++-23')
}
if ($env:CI_PRESET -eq 'release') {
  $qt = "-DCMAKE_PREFIX_PATH=$env:QT_ROOT_DIR"
} else {
  $qt = '-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=TRUE'
}
cmake --preset $env:CI_PRESET @compilers $qt `
  "-DPACKSTACKER_VERSION=$env:PACKSTACKER_VERSION" `
  "-DVCPKG_TARGET_TRIPLET=$env:TRIPLET" "-DVCPKG_HOST_TRIPLET=$env:TRIPLET"
