#version 120

attribute vec2 pos; // vertex position in cell

void main() {
    gl_Position = vec4(pos, 0, 1);
}
