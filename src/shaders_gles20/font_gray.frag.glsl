/* See LICENSE for license information. */

#version 100
precision mediump float;

uniform vec3      clr;
uniform sampler2D tex;

varying vec2 tex_coord;

void main() {
    float s = texture2D(tex, tex_coord).r;
    gl_FragColor = vec4(clr,s);
}
