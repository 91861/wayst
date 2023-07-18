#version 120

uniform vec4 clr;

void main() {
    gl_FragData[0] = clr;
}

