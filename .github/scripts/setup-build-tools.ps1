$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
. "$PSScriptRoot/download.ps1"

$root = Join-Path $env:RUNNER_TEMP build-tools
New-Item -ItemType Directory -Path $root | Out-Null
$version = '4.2.3'
if ($IsWindows) {
  $cmake = "cmake-$version-windows-x86_64"
  $extension = '.zip'
  $cmakeDigest = 'eb4ebf5155dbb05436d675706b2a08189430df58904257ae5e91bcba4c86933c'
  $ninja = 'ninja-win.zip'
  $ninjaDigest = 'beb3c1bfcd01d352cb46586a4677b0a49a93b7bb668cfa5d0695283e1d8efb61'
} else {
  $cmake = "cmake-$version-linux-x86_64"
  $extension = '.tar.gz'
  $cmakeDigest = '5bb505d5e0cca0480a330f7f27ccf52c2b8b5214c5bba97df08899f5ef650c23'
  $ninja = 'ninja-linux.zip'
  $ninjaDigest = '46aa8ad0a431e9b6e39f6ca0abc47bf8b13be094e3ac7d0f6d39e94bbdc746f9'
}
$cmakeArchive = Join-Path $root "$cmake$extension"
$ninjaArchive = Join-Path $root $ninja
Get-CiArchive "https://github.com/Kitware/CMake/releases/download/v$version/$cmake$extension" `
  $cmakeArchive $cmakeDigest
Get-CiArchive "https://github.com/ninja-build/ninja/releases/download/v1.13.0/$ninja" `
  $ninjaArchive $ninjaDigest
if ($IsWindows) {
  Expand-Archive -LiteralPath $cmakeArchive -DestinationPath $root
} else {
  tar -xf $cmakeArchive -C $root
}
Expand-Archive -LiteralPath $ninjaArchive -DestinationPath $root
if (-not $IsWindows) {
  chmod +x "$root/ninja"
}
Remove-Item -LiteralPath $cmakeArchive, $ninjaArchive
"$root/$cmake/bin" >> $env:GITHUB_PATH
$root >> $env:GITHUB_PATH
