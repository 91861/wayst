/* See LICENSE for license information. */

#version 100
precision mediump float;

uniform sampler2D tex;

varying vec2 tex_coord;

void main() {
    vec3 c = texture2D(tex,tex_coord).rgb;
    float a = length(c)/length(vec3(1,1,1));
    gl_FragColor = vec4(c,a);
}
