/* See LICENSE for license information. */

#version 120

uniform vec3 clr;  // font color
uniform vec4 bclr; // blend(background) color

uniform sampler2D tex;

varying vec2 tex_coord;

void main() {
    float c      = texture2D(tex, tex_coord).r;
    gl_FragDepth = 1.0 - c;
    gl_FragColor = vec4(mix(bclr.rgb * bclr.a, clr, c), bclr.a + c * (1.0 - bclr.a));
}
