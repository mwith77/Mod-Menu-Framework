# Mod Menu Framework

Gives mods a configuration menu inside The Last of Us Part I's own menus. It
adds a Mod Configuration entry to the main menu and the pause menu, built from
the game's own menu assets so it matches the shipped interface. Selecting it
lists one page per mod; opening a mod's page lists that mod's settings.

A mod adds its page by dropping one `.menupage` file, which describes the mod's
settings and names the mod's ini. The framework does the menu work and edits
that ini directly, so a mod needs no menu code of its own.

Version 1.0.2, for `tlou-i.exe` 1.1.5.0. It loads through the
[TLOU Plugin Loader](../TLOU%20Plugin%20Loader) and does nothing without it.

## Files

| File | Contents |
| --- | --- |
| `mod_menu_framework.cpp` | The framework: builds the menu pages and edits each mod's ini. |
| `tlou_plugin_sdk.h` | The loader header the framework includes. |
| `mod_menu_framework.ini` | The framework's own settings (the Mod Configuration entry's position). |
| `Mod Menus\mod_menu_framework.menupage` | The framework's own settings page. |
| `Mod Menus\example_mod.menupage`, `example_mod.ini` | A worked example page and the ini it edits, to write your own from. |
| `Game Menus\options.menupage`, `Game Menus\Menu Templates\` | The layouts the framework builds its menus from. |
| `build_mod_menu_framework.bat` | Builds the DLL. |

## Building

From the x64 Native Tools Command Prompt for Visual Studio, which provides
`cl`, run `build_mod_menu_framework.bat` in this folder. It builds
`mod_menu_framework.dll`.

## Installing

Laid out as it goes into the game folder, under a folder of its own in `Mods`:

    Mods\Mod Menu Framework\                mod_menu_framework.dll and its ini
    Mods\Mod Menu Framework\Mod Menus\      mod_menu_framework.menupage
    Mods\Mod Menu Framework\Game Menus\     options.menupage and Menu Templates

It needs the TLOU Plugin Loader, which loads it. A mod adds its page by putting
its `.menupage` in `Mods\Mod Menu Framework\Mod Menus`.

Uninstalling is deleting `Mods\Mod Menu Framework`. Nothing in a save depends on
the framework.

## Writing a mod page

A `.menupage` names the mod, points at the mod's ini, and lists each setting as
a control (a slider, a toggle, a selector) with its ini key, default and
description. The framework draws the page and writes each change straight to the
named ini. The mod reads its ini as it normally would.
`Mod Menus\example_mod.menupage`, with `example_mod.ini`, is a worked example to
write your own from.

## Logs

The framework keeps its log in `Mods\Mod Menu Framework\Logs` and writes only
failures and errors. Each log holds one run: at start, the previous run's log is
moved to `Logs\Archive`.
