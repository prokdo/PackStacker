set -euo pipefail
curl --fail --silent --show-error --location https://apt.llvm.org/llvm-snapshot.gpg.key |
  gpg --dearmor | sudo tee /usr/share/keyrings/llvm.gpg >/dev/null
echo 'deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-23 main' |
  sudo tee /etc/apt/sources.list.d/llvm.list >/dev/null
sudo apt-get update
case "$1" in
  format) packages=(clang-format-23) ;;
  headless) packages=(clang-23 clang-tidy-23 g++-14) ;;
  gui) packages=(clang-23 clang-tidy-23 g++-14 libegl-dev libgl-dev libfontconfig1
    libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-keysyms1 libxcb-shape0
    xvfb xauth xdotool 7zip) ;;
  *) exit 2 ;;
esac
sudo apt-get install -y --no-install-recommends "${packages[@]}"
