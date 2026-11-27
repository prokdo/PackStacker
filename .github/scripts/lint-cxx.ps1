$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$tools = @()
if ($IsWindows) {
  $tidy = 'clang-tidy'
  $tools = @('--extra-arg-before=--driver-mode=g++',
    '--extra-arg-before=--target=x86_64-w64-windows-gnu',
    "--extra-arg-before=--sysroot=$env:MINGW_ROOT")
} else {
  $tidy = 'clang-tidy-23'
}
$build = "build/$env:CI_PRESET"
$sourceRoots = @('src', 'tests') | ForEach-Object {
  (Join-Path $PWD $_).Replace('\', '/') + '/'
}
$sources = @(Get-Content "$build/compile_commands.json" -Raw | ConvertFrom-Json |
  ForEach-Object { $_.file.Replace('\', '/') } |
  Where-Object { $_.StartsWith($sourceRoots[0]) -or $_.StartsWith($sourceRoots[1]) } |
  Sort-Object -Unique)
if ($sources.Count -eq 0) { throw 'No project sources in compilation database' }
& $tidy --version
& $tidy --verify-config
& $tidy -p $build --quiet @tools @sources
