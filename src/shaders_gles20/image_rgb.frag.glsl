/* See LICENSE for license information. */

#version 100
precision mediump float;

uniform sampler2D tex;

varying vec2 tex_coord;

void main() {
    gl_FragColor = texture2D(tex, tex_coord);
}
