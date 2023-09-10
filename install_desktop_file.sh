#!/bin/sh

icon_name='utilities-terminal'
custom_icon=0

exec_fname='wayst'
app_name='Wayst'

for i in "$@"; do
    case $i in
        --custom-icon*)
        icon_name="wayst"
        custom_icon=1
        shift;;
        --exec=*) exec_fname="${i#*=}";shift;;
        --app=*)  app_name="${i#*=}"  ;shift;;
    esac
done

desktop_file_name="$(echo "$app_name" | tr '[:upper:]' '[:lower:]').desktop"

content="[Desktop Entry]
Version=1.0
Type=Application
Exec=$exec_fname
Icon=$icon_name
Terminal=false
Categories=System;TerminalEmulator;
Name=$app_name
GenericName=Terminal
Comment=A simple terminal emulator
X-ExecArg=-e
Keywords=$app_name;terminal;"

echo "$content" > "$(dirname $0)/$desktop_file_name"
xdg-desktop-menu install "$(dirname $0)/$desktop_file_name" --novendor

install_icons() {
    tgt_fname="$icon_name.png"
    theme="/usr/share/icons/hicolor"

    for icon_png in $(dirname $0)/icons/pngs/*.png; do
        size=$(basename ${icon_png%.*})
        tgt_file="$theme/""$size""x""$size/apps/$tgt_fname"
        cp "$icon_png" "$tgt_file"
    done

    cp "$(dirname $0)/icons/$icon_name.svg" "$theme/scalable/apps/"
    gtk-update-icon-cache -f -t "$theme"
}

[ $custom_icon = 0 ] || install_icons

