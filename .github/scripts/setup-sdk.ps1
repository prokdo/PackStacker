param([Parameter(Mandatory)][ValidateSet('gui', 'headless')][string]$Profile)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
. "$PSScriptRoot/download.ps1"

function Install-QtComponent {
  param([string]$Repository, [string]$Component, [string]$QtPrefix)

  [xml]$metadata = (& $curl --fail --silent --show-error --location --retry 3 `
    --max-time 60 "$Repository/Updates.xml" | Out-String)
  $package = @($metadata.Updates.PackageUpdate | Where-Object { $_.Name -ceq $Component })
  if ($package.Count -ne 1 -or $package[0].Version -notmatch '^[0-9][0-9.-]*$') {
    throw "Cannot find Qt component $Component"
  }
  $archives = $package[0].DownloadableArchives -split ',\s*'
  foreach ($archive in $archives) {
    if ($archive -notmatch '^[a-zA-Z0-9_.-]+\.7z$') {
      throw "Invalid archive name for $Component"
    }
    $destination = $sdk
    if ($QtPrefix) {
      $operations = @($package[0].Operations.Operation | Where-Object {
        $_.name -ceq 'Extract' -and $_.Argument[1] -ceq $archive
      })
      if ($operations.Count -ne 1) {
        throw "Expected one extraction destination for $archive"
      }
      $prefix = "@TargetDir@/$QtPrefix"
      $target = [string]$operations[0].Argument[0]
      if ($target -cnotin @($prefix, "$prefix/bin", "$prefix/lib")) {
        throw "Unexpected extraction destination for ${archive}: $target"
      }
      $destination = Join-Path $sdk $target.Substring('@TargetDir@/'.Length)
    }
    $name = "$($package[0].Version)$archive"
    $url = "$Repository/$Component/$name"
    $digest = (& $curl --fail --silent --show-error --location --retry 3 `
      --max-time 60 "$url.sha1" | Out-String).Trim()
    if ($digest -notmatch '^[a-fA-F0-9]{40}$') {
      throw "Missing published checksum for $name"
    }
    Write-Host "Installing $name"
    $file = Join-Path $sdk $name
    Get-CiArchive $url $file $digest SHA1
    & $sevenZip x -y -bb0 -bso0 "-o$destination" $file
    Remove-Item -LiteralPath $file
  }
}

$sdk = Join-Path $env:RUNNER_TEMP Qt
New-Item -ItemType Directory -Path $sdk | Out-Null
$curl = (Get-Command curl -CommandType Application -TotalCount 1 -ErrorAction Stop).Source
$repo = 'https://download.qt.io/online/qtsdkrepository'
if ($IsWindows) {
  $platform = 'windows_x86'
  $arch = 'win64_mingw'
  $qtDir = 'mingw_64'
  $sevenZip = Join-Path $env:ProgramFiles '7-Zip/7z.exe'
  Install-QtComponent "$repo/$platform/desktop/tools_mingw1310" 'qt.tools.win64_mingw1310'
  $mingw = Join-Path $sdk Tools/mingw1310_64
  if (-not (Test-Path "$mingw/bin/g++.exe")) { throw "MinGW not found at $mingw" }
  "MINGW_ROOT=$mingw" >> $env:GITHUB_ENV
  "$mingw/bin" >> $env:GITHUB_PATH
} else {
  $platform = 'linux_x64'
  $arch = 'linux_gcc_64'
  $qtDir = 'gcc_64'
  $sevenZip = (Get-Command 7zz, 7z -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1).Source
}
if ($Profile -eq 'gui') {
  if ($env:QT_VERSION -notmatch '^\d+\.\d+\.\d+$') {
    throw "Invalid Qt version: $env:QT_VERSION"
  }
  $versionDigits = $env:QT_VERSION.Replace('.', '')
  $catalog = "qt6_$versionDigits"
  $qtPrefix = "$env:QT_VERSION/$qtDir"
  Install-QtComponent "$repo/$platform/desktop/$catalog/$catalog" "qt.qt6.$versionDigits.$arch" $qtPrefix
  $qt = Join-Path $sdk $qtPrefix
  if (-not (Test-Path "$qt/lib/cmake/Qt6/Qt6Config.cmake")) { throw "Qt SDK not found at $qt" }
  Set-Content -LiteralPath "$qt/bin/qt.conf" -Encoding ascii -Value '[Paths]', 'Prefix=..'
  "QT_ROOT_DIR=$qt" >> $env:GITHUB_ENV
  "$qt/bin" >> $env:GITHUB_PATH
}
