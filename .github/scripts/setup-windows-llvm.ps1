$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$tag = gh api --paginate 'repos/llvm/llvm-project/releases?per_page=100' `
  --jq '.[] | select(.draft == false and .prerelease == false) | .tag_name' |
  Where-Object { $_ -cmatch '^llvmorg-23\.[0-9]+\.[0-9]+$' } |
  Sort-Object { [version]($_ -creplace '^llvmorg-', '') } -Descending |
  Select-Object -First 1
if (-not $tag) { throw 'No stable LLVM 23 release found' }
$version = $tag -creplace '^llvmorg-', ''
$name = "LLVM-$version-win64.msi"
$release = gh api "repos/llvm/llvm-project/releases/tags/$tag" | ConvertFrom-Json
$asset = $release.assets | Where-Object { $_.name -ceq $name -and $_.state -eq 'uploaded' }
if (@($asset).Count -ne 1 -or $asset.digest -notmatch '^sha256:[0-9a-f]{64}$') {
  throw "No published SHA-256 for $name"
}
$msi = Join-Path $env:RUNNER_TEMP llvm.msi
$llvm = Join-Path $env:RUNNER_TEMP llvm
Invoke-WebRequest `
  "https://github.com/llvm/llvm-project/releases/download/$tag/$name" `
  -OutFile $msi
if ((Get-FileHash $msi -Algorithm SHA256).Hash -ne $asset.digest.Substring(7)) {
  throw 'Unexpected LLVM installer checksum'
}
$installer = Start-Process msiexec.exe -ArgumentList "/a `"$msi`" /qn TARGETDIR=`"$llvm`"" `
  -Wait -PassThru
if ($installer.ExitCode -ne 0) { throw "LLVM extraction failed: $($installer.ExitCode)" }
$tidy = Get-ChildItem $llvm -Recurse -Filter clang-tidy.exe | Select-Object -First 1
if (-not $tidy) { throw 'clang-tidy.exe not found' }
$tidy.DirectoryName >> $env:GITHUB_PATH
Remove-Item $msi
