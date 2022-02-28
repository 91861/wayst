/* See LICENSE for license information. */

#version 100
precision mediump float;

attribute vec4 coord;

uniform vec2 offset;

varying vec2 tex_coord;

void main() {
    tex_coord = coord.zw;
    gl_Position = vec4(coord.xy + offset, 0, 1);
}
