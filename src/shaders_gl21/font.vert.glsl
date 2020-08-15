/* See LICENSE for license information. */

#version 120

attribute vec4 coord; // (pos_x, pos_y, tex_coord_x, tex_coord_y)

varying vec2 tex_coord;

void main() {
  tex_coord = coord.zw;
  gl_Position = vec4(coord.xy, 0, 1);
}

