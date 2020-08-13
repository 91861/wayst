#!/bin/sh

app_name='Wayst'

for i in "$@"; do
case $i in
    --app=*)  app_name="${i#*=}"  ;shift;;
esac
done

desktop_file_name="$(echo "$app_name" | tr '[:upper:]' '[:lower:]').desktop"

if ls "$(dirname $0)/$desktop_file_name" 1> /dev/null 2>&1; then
    xdg-desktop-menu uninstall "$(dirname $0)/$desktop_file_name"
fi

icons="/usr/share/icons/hicolor/*/apps/wayst.??g"

if ls $icons 1> /dev/null 2>&1; then
    ls $icons
    rm -I $icons
fi

