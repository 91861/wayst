/* See LICENSE for license information. */


#version 120

attribute vec2 pos; // vertex position in cell

uniform vec2 mv;    // position offset

void main() {
    gl_Position = vec4(pos + mv, 0, 1);
}
