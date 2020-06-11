/* See LICENSE for license information. */


#version 120

uniform vec3 clr;  // font color

uniform sampler2D tex;

varying vec2 tex_coord;

void main() {

    vec3 c = texture2D(tex, tex_coord).rgb;

    gl_FragData[0] = vec4(c.r * clr.r,
                          c.r * clr.g,
                          c.r * clr.b,
                          c.r);
}
