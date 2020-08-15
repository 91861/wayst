/* See LICENSE for license information. */

#version 120

uniform sampler2D tex;

varying vec2 tex_coord;

void main() {
    vec3 c = texture2D(tex, tex_coord).rgb;
    // depth is in range 0..1 where, by default, a lower value that passes the test
    gl_FragDepth = 1.0 - length(c) / length(vec3(1, 1, 1));
    gl_FragColor = vec4(c, 1);
}
