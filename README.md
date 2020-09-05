
<p align="center">
  <img src=".github/waystScrot.png" alt="screenshot" />
</p>

# About
Simple terminal emulator for Wayland and X11 with OpenGL rendering and minimal dependencies.

**Work in progress, expect bugs and missing features!**


# Features
* Unicode support
* Text reflow
* 24-bit colors
* All text properties (squiggly underline, blinking, overline etc.)
* Resizable font
* Subpixel font rendering
* Mouse reporting
* Scrollback
* Mouse text selection
* Clipboard
* Configurable keybindings


# To-Do
* All xterm and vte control sequences
* Ligatures
* Graphics


# Building
```shell
make
make install
```

#### Dependencies:
* OpenGL >= 2.1
* freetype >= 2.10
* fontconfig
* xkbcommon [wayland]
* utf8proc [optional]

To build without X11 or Wayland support set ```window_protocol=wayland``` or ```window_protocol=x11``` respectively. With both backends enabled wayst will default to wayland. You can force X11 mode with the ```xorg-only``` option.

To build in debug mode set ```mode=debugoptimized```.


## Installation from AUR

You can install [wayst-git](https://aur.archlinux.org/pkgbase/wayst-git/) from AUR (arch user repository)

```shell
yay -S wayst-git
```


# Usage

#### Configuration:
All option can be set in a configuration file or passed as command line arguments. To see all supported options run ```wayst --help```.\
Wayst will look for: ```$XDG_CONFIG_HOME/wayst/config``` or ```$HOME/.config/wayst/config```.

Example:
```ini
# '#' starts a line comment
# Strings with spaces need double quotes (use \" for " and \\ for \).

style-bold = "Semibold"

# Set a list of primary fonts (All available styles will be loaded)
font = [
    # You can set codepoint ranges to which a given font should be applied.
    # Here we define <min>..u+24ff and u+2580..<max> to exclude the unicode
    # box drawing block, those characters will be loaded (if present) from
    # the following fonts in this list.
    "mononoki:..u+24ff:u+2580..",

    "sauce code pro nerd font",

    # Mixing ttf/otf with bitmap fonts is fine, as long as all ASCII
    # characters use the same format.
    #
    # You can set a size offset to keep a font smaller/larger than
    # the global font size.
    "Terminus:-3"
]

# Or use a single font
# font = "IBM Plex Mono"

# Set a list of 'symbol' fonts (Only the 'Regular' style will be loaded)
font-symbol = [
    "Noto Sans Symbols",
    "FontAwesome"
]

font-color = "Noto Color Emoji"

font-size = 10
dpi = 96

# Load one of the default colorschemes
colorscheme = "wayst"

# Overwrite parts of the colorscheme
fg-color = "#c7eeff"
bg-color = "#000000ee"

term = "xterm-256color"
no-flash = true

title = "Terminal"
title-format = "%2$s - %1$s"  #  <set by program> - Terminal
# title-format = "%s [%s]"    #  Terminal [<set by program>]
# title-format = "%2$s"       #  <set by program>


# xorg keysym names are case sensitive!
bind-key-debug=Ctrl+Shift+Return
bind-key-enlarge=Ctrl+Shift+equal
bind-key-shrink=Ctrl+Shift+minus
bind-key-copy=Ctrl+Shift+y
bind-key-paste=Ctrl+Shift+p
```

#### Default Keybindings:
Keys|Action|
 --- | ---
```Ctrl```+```Shift```+```c```     | Copy to clipboard
```Ctrl```+```Shift```+```v```     | Paste from clipboard
```Ctrl```+```Shift```+```=```     | Increase font size
```Ctrl```+```Shift```+```-```     | Decrease font size
```Ctrl```+```Shift```+```u```     | Enter unicode character by hex code
```Ctrl```+```Shift```+```k```     | Enter vi-like keyboard select mode
```Ctrl```+```Shift```+```d```     | Start new instance in active work directory (set by OSC 7)
```Ctrl```+```Shift```+```/```     | Output debug information to stdout
```LMB```                          | Select text
```RMB```                          | Change selected region
```Shift```+```LMB```              | Select text in mouse reporting mode
```Ctrl``` + ```LMB```             | Box select


# License
MIT
