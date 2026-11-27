function Get-CiArchive {
  param(
    [string]$Uri,
    [string]$File,
    [string]$Digest,
    [ValidateSet('SHA256', 'SHA1')][string]$Algorithm = 'SHA256'
  )

  $curl = (Get-Command curl -CommandType Application -ErrorAction Stop).Source
  & $curl --fail --silent --show-error --location --retry 3 --max-time 600 --output $File $Uri
  if ((Get-FileHash -LiteralPath $File -Algorithm $Algorithm).Hash -ne $Digest) {
    throw "Unexpected $Algorithm checksum for $Uri"
  }
}
