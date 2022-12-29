/* See LICENSE for license information. */

#version 100
precision mediump float;

uniform vec4 clr;  // color
uniform vec4 bclr; // blend(background) color
uniform vec4 cir;  // [x, y, r, fade]

varying vec2 fpos;

void main() {
    gl_FragColor = mix(bclr, clr, smoothstep(cir.z + cir.w, cir.z - cir.w, distance(cir.xy, fpos)));
}
