# Mod Menu Framework

Gives mods a configuration menu inside the own menus of The Last of Us Part I
and The Last of Us Part II. It
adds a Mod Configuration entry to the main menu and the pause menu, built from
the game's own menu assets so it matches the shipped interface. Selecting it
lists one page per mod; opening a mod's page lists that mod's settings.

A mod adds its page by dropping one `.menupage` file, which describes the mod's
settings and names the mod's ini. The framework does the menu work and edits
that ini directly, so a mod needs no menu code of its own.

Version 1.2.0, for `tlou-i.exe` 1.1.5.0 and `tlou-ii.exe` 1.6.0 (in
`tlou-ii-l.exe` it does nothing yet, and says so in its log). It loads through
the [TLOU Plugin Loader](https://github.com/mwith77/TLOU-Plugin-Loader), version
1.1.0 or later, and does nothing without it.

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
`cl` and `rc`, run `build_mod_menu_framework.bat` in this folder. It builds
`mod_menu_framework.dll`.

## Installing

Laid out as it goes into the game folder, under a folder of its own in `Mods`:

    Mods\Mod Menu Framework\                mod_menu_framework.dll
    Mods\Mod Menu Framework\Mod Menus\      mod_menu_framework.menupage
    Mods\Mod Menu Framework\Game Menus\     options.menupage and Menu Templates

At its first start the framework writes `mod_menu_framework.ini` beside its
DLL where it is missing or empty, and adds any setting it lacks, so updating
never resets it. It needs the TLOU Plugin Loader, which loads it. A mod adds its
page by putting its `.menupage` in `Mods\Mod Menu Framework\Mod Menus`.

Uninstalling is deleting `Mods\Mod Menu Framework`. Nothing in a save depends on
the framework.

## Writing a mod page

A `.menupage` names the mod, points at the mod's ini, and lists each setting as
a control with its ini key, default and description. The framework draws the
page and writes each change straight to the named ini. The mod reads its ini as
it normally would. `Mod Menus\example_mod.menupage`, with `example_mod.ini`, is
a worked example to write your own from.

A control's `ControlType` is one of these:

| ControlType | What it is |
| --- | --- |
| `Toggle` | Two values, such as OFF and ON. |
| `Selector` | A list of named values. |
| `Slider` | A number between a minimum and a maximum, in steps. `SliderValueText` can show text in place of the number, for example `[IF(VALUE > 30, "Persistent", VALUE)]`. |
| `Preset` | A selector whose options each set several settings at once (`PresetValues`), followed by Custom, which puts back the values held before a preset was chosen. |
| `Keybind` | A key, kept in the ini as its Windows virtual-key code (F9 is `0x78`) and set by holding it, as on the game's Customise Controls page. `KeybindAllowMenuKeys` and `KeybindAllowGameplayKeys` say whether it may take a key the game already uses in its menus or in play; by default menu keys are allowed and gameplay keys are refused. |

`Preset`, `Keybind` and `SliderValueText` are new in version 1.1.0. A page that
uses them needs 1.1.0 or later; every page written for 1.0.2 works unchanged.
`PresetOff: ["OFF" = {name = value, ...}]` (new in 1.2.0) gives a Preset an Off,
the settings' defaults, reached only by Reset to Defaults.

## Logs

The framework keeps one log, `Mods\Mod Menu Framework\mod_menu_framework.log`,
newest line first, at most 1 MB, and writes only failures and errors.
