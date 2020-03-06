# About
Simple terminal emulator for Wayland and X11 with OpenGL rendering and minimal dependencies.

**Warning:** This project is work in progress, expect bugs and missing features!

# Features
* Unicode support
* Subpixel font rendering
* Text reflow
* 24-bit colors
* All text properties (squiggly underline, blinking, overline etc.)
* Mouse reporting
* Scrollback
* Mouse text selection
* Clipboard

# To-Do
* Font reloading
* Sixel graphics
* Configurable keys

# Building
```shell
make
make install
```

To build without X11 or Wayland support set ```window_protocol=wayland``` or ```window_protocol=x11``` respectively. With both backends enabled wayst will default to wayland. You can force X11 mode with the ```xorg-only``` option. To build in debug mode set ```mode=debugoptimized```.

###### Dependencies:
* OpenGL >= 2.1
* freetype >= 2.10
* fontconfig
* xkbcommon [wayland]

# Usage

### Configuration:
All option can be set in a configuration file or passed as command line arguments. To see all supported options run ```wayst --help```.\
Wayst will look for: ```$XDG_CONFIG_HOME/wayst/config``` or ```/$HOME/.config/wayst/config```.

Example:
```
# '#' starts a line comment
# Use double quotes for strings with spaces, \" for ", \# for # and \\ for  \.

font="mononoki"
font-size=10
dpi=96
colorscheme=wayst
title="Terminal"
term="xterm256-color"
dynamic-title=true
title-format = "%2$s - %1$s"       # -> user@host:~ - $title
```

### Keybindings:

Currently keybindings can't be reconfigured.

Keys|Action|
 --- | ---
```ctrl```+```shift```+```c``` / ```ctrl```+```shift```+```y```| Copy to clipboard
```ctrl```+```shift```+```p``` | Paste clipboard
```LMB``` | Select text
```shift```+```LMB``` | Select text in mouse reporting mode
```ctrl``` + ```LMB``` | Box select
```ctrl```+```shift```+```enter``` | Output debug information to stdout


# License
MIT
