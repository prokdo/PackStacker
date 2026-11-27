# PackStacker

[[🇬🇧 EN]](README.md) | [[🇷🇺 RU]](README_RU.md)

[![Build & Tests](https://github.com/prokdo/PackStacker/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/prokdo/PackStacker/actions/workflows/ci.yml?query=branch%3Amain)

Combine Minecraft Java Edition resource packs **or** data packs into one ZIP archive or directory.

## Features

- Order packs by priority in the GUI or on the command line.
- Merge supported Minecraft JSON resources or override the behavior with your own rules.
- Read ZIP archives and folders without modifying them. Export to a new path without overwriting an existing result.

## Download and run

Check [Releases](https://github.com/prokdo/PackStacker/releases) for Linux x64 and Windows x64 builds. Choose `gui` for the desktop app and CLI, or `headless` for just the CLI without Qt. Extract the entire archive and run `packstacker-gui` or `packstacker-cli --help` from `bin/` (use `.exe` on Windows).

Each operation uses one pack type and one pack format supported by all inputs. PackStacker does not convert packs between Minecraft versions.

## Documentation

- [Wiki](https://github.com/prokdo/PackStacker/wiki): quick start, GUI, CLI, merge rules and troubleshooting.
- [Build from source, tests and CI](docs/Development.md).
- [License](LICENSE).
