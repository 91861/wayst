/* See LICENSE for license information. */

#version 100
precision mediump float;

uniform vec3 clr;  // font color

uniform sampler2D tex;

varying vec2 tex_coord;

void main() {
    vec3 c = texture2D(tex,tex_coord).rgb;
    gl_FragColor = vec4(clr.r*c.r,
                        clr.g*c.r,
                        clr.b*c.r,
                        (1.0-c.r)*(1.0-c.r));
}
