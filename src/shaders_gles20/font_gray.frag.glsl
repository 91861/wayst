/* See LICENSE for license information. */

#version 100
precision mediump float;

uniform vec3      clr;
uniform vec4      bclr;
uniform sampler2D tex;

varying vec2 tex_coord;

void main() {
    float s      = texture2D(tex, tex_coord).r;
    gl_FragColor = vec4(mix(bclr.rgb * bclr.a, clr, s), bclr.a + (1.0 / s) * (1.0 - bclr.a));
}
