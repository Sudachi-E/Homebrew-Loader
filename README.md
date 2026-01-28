# Homebrew Loader

An Aroma plugin for loading homebrew though the config menu:
- Browse and launch homebrew from the SD card `sd:/wiiu/apps`

## Features
  - Scans `sd:/wiiu/apps` recursively for `.wuhb` and `.rpx` homebrew
  - Navigate apps with D-Pad left/right; launch with `A`
  - Add a quick favorite with `Y` to create a dedicated menu item
  - Remove a quick favorite with `X` on the quick item

  ## Usage
  - Open the plugin config menu
  - Select “Launch homebrew” and browse with D-Pad left/right
  - Press `A` to launch the selected app
  - Press `Y` on selected homebrew to create a quick favorite for the current app
    - Close and reopen the config menu to see the new favorite
  - Press `X` on a quick favorite to remove it
    - Close and reopen the config menu for the list to update

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
- Thanks to [@Maschell](https://github.com/Maschell) for WUPS and WUMS
- The [@devkitpro](https://github.com/devkitPro) team for WUT
