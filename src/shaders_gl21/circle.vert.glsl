/* See LICENSE for license information. */

#version 120

attribute vec2 pos; // (pos_x, pos_y)

varying vec2 fpos;

void main() {
    fpos        = pos;
    gl_Position = vec4(pos, 0, 1);
}
