# Homebrew Loader

An Aroma plugin that opens an overlay menu to browse and launch homebrew from the SD card via a button combo (default: L + R + D-Pad Down).

The button combo can be changed in the Aroma Config Menu.

## Features
- Scans `sd:/wiiu/apps` recursively for `.wuhb` and `.rpx` homebrew

##

## Requirements
- devkitPro toolchain installed and `DEVKITPRO` environment variable set
- Libraries installed:
  - [wut](https://github.com/devkitPro/wut)
  - [wups](https://github.com/Maschell/WiiUPluginSystem)
  - [wums](https://github.com/Maschell/wums)

## Build
- From project root run:
  - `make`

## Installation
- Copy `HomebrewLoader.wps` to `sd:/wiiu/environments/aroma/plugins`

## Credits
- Thanks to [@Maschell](https://github.com/Maschell) for WUPS, WUMS
- The [@devkitpro](https://github.com/devkitPro) team for WUT
