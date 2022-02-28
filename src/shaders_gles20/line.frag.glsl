/* See LICENSE for license information. */

#version 100
precision mediump float;

uniform vec3 clr;

void main() {
    gl_FragColor =  vec4(clr, 1);
}

