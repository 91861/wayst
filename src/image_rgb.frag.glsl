/* See LICENSE for license information. */


#version 120

uniform sampler2D tex;

varying vec2 tex_coord;

void main() {
    gl_FragData[0] = texture2D(tex, tex_coord);
}
