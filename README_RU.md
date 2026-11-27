# PackStacker

[[🇬🇧 EN]](README.md) | [[🇷🇺 RU]](README_RU.md)

[![Сборка и тесты](https://github.com/prokdo/PackStacker/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/prokdo/PackStacker/actions/workflows/ci.yml?query=branch%3Amain)

Объединяйте наборы ресурсов **или** наборы данных Minecraft Java Edition в один ZIP-архив или папку.

## Возможности

- Расставляйте паки по приоритету в графическом интерфейсе или командной строке.
- Объединяйте поддерживаемые JSON-ресурсы Minecraft или задавайте собственные правила.
- Читайте ZIP-архивы и папки без изменения исходников. Сохраняйте результат по новому пути без перезаписи.

## Скачивание и запуск

Ищите сборки для Linux x64 и Windows x64 в разделе [Releases](https://github.com/prokdo/PackStacker/releases). Выберите `gui` для графического приложения и CLI или `headless` для одного CLI без Qt. Распакуйте архив целиком и запустите `packstacker-gui` или `packstacker-cli --help` из `bin/` (в Windows добавьте `.exe`).

За одну операцию можно объединить паки одного типа, если все они поддерживают выбранный pack format. PackStacker не конвертирует паки между версиями Minecraft.

## Документация

- [Wiki](https://github.com/prokdo/PackStacker/wiki/Home-RU): быстрый старт, GUI, CLI, правила объединения и решение проблем.
- [Сборка из исходников, тесты и CI](docs/Development-RU.md).
- [Лицензия](LICENSE).
