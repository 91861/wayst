/* See LICENSE for license information. */


#version 120

uniform vec3 clr;  // font color
uniform vec3 bclr; // blend(background) color
uniform sampler2D tex;

varying vec2 tex_coord;

void main() {

    vec3 c = texture2D(tex, tex_coord).rgb;

    gl_FragData[0] = vec4(mix(bclr.r, clr.r, c.r),
                          mix(bclr.g, clr.g, c.g),
                          mix(bclr.b, clr.b, c.b),
                          length(c) * 10000);
}
