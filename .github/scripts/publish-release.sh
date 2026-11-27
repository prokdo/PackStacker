set -euo pipefail

archives=(
  release-assets/packstacker-linux-x64-gui.tar.gz
  release-assets/packstacker-linux-x64-headless.tar.gz
  release-assets/packstacker-windows-x64-gui.zip
  release-assets/packstacker-windows-x64-headless.zip
)
for archive in "${archives[@]}"; do
  if [[ ! -s "$archive" ]]; then
    echo "Missing verified archive: $archive" >&2
    exit 1
  fi
done
gh release create "$RELEASE_TAG" --verify-tag --generate-notes --draft "${archives[@]}"
gh release edit "$RELEASE_TAG" --draft=false
