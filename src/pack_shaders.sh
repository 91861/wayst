#!/bin/bash
# find glsl shaders in given directory and export them as a C header removing comments and whitespace wherever possible
# TODO: grep for uniform declarations and generate #define-s with their indicies 

function process {
    prepr_lines="`echo "$2"|grep .|grep '#'|sed 's/.*/"&\\\\\\\n"/'`"
    no_comments="`echo "$2"|gcc -fpreprocessed -w -E -P -x c -w - 2> /dev/null`"
    no_spaces="`echo "$no_comments"|sed "s/^[ \t]*//"|sed 's/[[:space:]]\([^[:alnum:]]\)/\1/g'|sed 's/\([^[:alnum:]]\)[[:space:]]/\1/g'`"
    in_quotes="`echo "$no_spaces"|sed 's/.*/"&"/'`"
    printf "\n\nconst char*\n$3$4 =\n$prepr_lines\n$in_quotes;\n"
}

printf "// This file was autogenerated from src/%s/*.glsl. Run \`make shaders\` to update." $1

for file in $(dirname $0)/$1/*.geom.glsl; do
    [[ -f "$file" ]] && process res "$(cat $file)" $(basename ${file:2:-10}) '_gs_src'
done

for file in $(dirname $0)/$1/*.vert.glsl; do
    [[ -f "$file" ]] && process res "$(cat $file)" $(basename ${file:2:-10}) '_vs_src'
done

for file in $(dirname $0)/$1/*.frag.glsl; do
    [[ -f "$file" ]] && process res "$(cat $file)" $(basename ${file:2:-10}) '_fs_src'
done
