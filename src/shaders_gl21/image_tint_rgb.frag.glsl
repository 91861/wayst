/* See LICENSE for license information. */


#version 120

uniform sampler2D tex;
uniform vec3 tint;

varying vec2 tex_coord;

void main() {
    gl_FragColor = texture2D(tex, tex_coord) * vec4(tint, 1.0);
}
