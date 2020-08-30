/* See LICENSE for license information. */

#version 120

uniform vec3 clr;  // font color
uniform vec4 bclr; // blend(background) color

uniform sampler2D tex;

varying vec2 tex_coord;

void main() {

    vec3 c = texture2D(tex, tex_coord).rgb;

    gl_FragDepth = 1.0 - length(c) / length(vec3(1, 1, 1));
    gl_FragColor = vec4(mix(bclr.r, clr.r, c.r),
                        mix(bclr.g, clr.g, c.r),
                        mix(bclr.b, clr.b, c.r),
                        bclr.a + c.r);
}
