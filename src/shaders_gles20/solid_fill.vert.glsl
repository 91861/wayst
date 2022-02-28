/* See LICENSE for license information. */

#version 100
precision mediump float;

attribute vec2 pos;

void main() {
    gl_Position = vec4(pos, 0, 1);
}
