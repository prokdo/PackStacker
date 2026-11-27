$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = Join-Path $env:RUNNER_TEMP vcpkg
$cache = Join-Path $env:RUNNER_TEMP vcpkg-cache
$registry = (Get-Content vcpkg-configuration.json -Raw | ConvertFrom-Json).'default-registry'
git init $root
git -C $root fetch --depth=1 $registry.repository $registry.baseline
git -C $root checkout --detach FETCH_HEAD
if ($IsWindows) {
  & "$root/bootstrap-vcpkg.bat" -disableMetrics
} else {
  & "$root/bootstrap-vcpkg.sh" -disableMetrics
}
New-Item -ItemType Directory -Path $cache -Force | Out-Null
"VCPKG_ROOT=$root" >> $env:GITHUB_ENV
"VCPKG_DEFAULT_BINARY_CACHE=$cache" >> $env:GITHUB_ENV
"VCPKG_DEFAULT_TRIPLET=$env:TRIPLET" >> $env:GITHUB_ENV
"VCPKG_DEFAULT_HOST_TRIPLET=$env:TRIPLET" >> $env:GITHUB_ENV
