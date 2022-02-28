/* See LICENSE for license information. */

#version 100
precision mediump float;

uniform vec3 clr; // font color
uniform vec4 bclr; // blend(background) color
uniform sampler2D tex;

varying vec2 tex_coord;

void main()
{
    vec3 c = texture2D(tex, tex_coord).rgb;
    float a = 1.0 - length(c) / length(vec3(1, 1, 1));
    gl_FragColor = vec4(mix(bclr.r, clr.r, c.r),
                        mix(bclr.g, clr.g, c.g),
                        mix(bclr.b, clr.b, c.b),
                        a*a);
}
