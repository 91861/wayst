#!/bin/sh
this_dir=$(dirname $0)
input_file="$this_dir/wayst.svg"
output_dir='pngs'

mkdir -p "$this_dir/$output_dir"

for_dim() {
    inkscape -w "$1" -h "$1" "$input_file" --export-filename "$this_dir/$output_dir/$1.png"
}

for_dim 48
for_dim 192
for_dim 512

