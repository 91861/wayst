
<p align="center">
  <img src=".github/waystScrot.png" alt="screenshot" />
</p>

# About
Simple terminal emulator for Wayland and X11 with OpenGL rendering and minimal dependencies.

**This is roughly alpha quality, expect bugs!**

### Features
* Unicode support
* Text reflow
* 24-bit colors
* Dynamic colors
* All text properties (squiggly underline, blinking, overline etc.)
* Resizable font
* Subpixel antialiasing
* Mouse reporting
* Scrollback
* Mouse text selection
* Clipboard
* Configurable keybindings
* Clickable links, OSC 8 links
* Command history and marks[*](https://github.com/91861/wayst#shell-integration)
* [Terminal image protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/) and sixel graphics (experimental)

### Limitations
* UTF8 mode only
* No Bidi support
* No font ligatures

### To-Do
* Search
* Ibus support
* Single instance multi window mode
* All xterm and vte control sequences

# Building
```shell
make
make install
```

### Dependencies:
* OpenGL >= 2.1/ES 2.0
* freetype >= 2.10
* fontconfig
* xkbcommon [wayland]
* xrandr [X11]
* utf8proc [optional]
* notify-send [optional]

### Build options
To build without X11 or Wayland support set ```window_protocol=wayland``` or ```window_protocol=x11``` respectively. With both backends enabled wayst will default to wayland. You can force X11 mode with the ```xorg-only``` option.

To target OpenGL ES 2.0 instead of OpenGL 2.1 set ```renderer=gles20```.

To build without libutf8proc set ```libutf8proc=off```.

To build with debuging symbols set ```mode=debug``` or ```mode=debugoptimized```.


## Installation from AUR

You can install [wayst-git](https://aur.archlinux.org/pkgbase/wayst-git/) from AUR (arch user repository)

```shell
yay -S wayst-git
```


# Usage

#### Configuration file
All option can be set in a configuration file or passed as command line arguments. To see all supported options run ```wayst --help```.

Wayst will look for: ```$XDG_CONFIG_HOME/wayst/config``` or ```$HOME/.config/wayst/config```.
For an example configuration file see ```config.example```.


#### Shell integration
Wayst can use [iTerm2 shell integration sequences](https://iterm2.com/documentation-shell-integration.html) or
[VTE prompt notifications](https://bugzilla.gnome.org/show_bug.cgi?id=711059) to track command history.

#### Default Keybindings
Keys|Action|
 --- | ---
```Ctrl```+```Shift```+```c```            | Copy to clipboard
```Ctrl```+```Shift```+```x```            | Copy output of last command to clipboard[*](https://github.com/91861/wayst#shell-integration)
```Ctrl```+```Shift```+```v```            | Paste from clipboard
```Ctrl```+```Shift```+```=```            | Increase font size
```Ctrl```+```Shift```+```-```            | Decrease font size
```Ctrl```+```Shift```+```Up/Down```      | Scroll
```Ctrl```+```Shift```+```Page Up/Down``` | Scroll by page
```Ctrl```+```Shift```+```Left/Right```   | Jump to previous/next command output or mark[*](https://github.com/91861/wayst#shell-integration)
```Ctrl```+```Shift```+```u```            | Enter unicode character by hex code
```Ctrl```+```Shift```+```k```            | Enter vi-like keyboard select mode
```Ctrl```+```Shift```+```d```            | Start new instance in active work directory (set by OSC 7)
```Ctrl```+```Shift```+```F12```          | HTML screen dump
```Ctrl```+```Shift```+```\```            | Pipe to external program
```LMB```                                 | Select text
```RMB```                                 | Change selected region
```MMB```                                 | Paste from primary selection
```Shift```+```LMB```                     | Select text in mouse reporting mode
```Ctrl``` + ```LMB```                    | Open link/Box select

# License
MIT
