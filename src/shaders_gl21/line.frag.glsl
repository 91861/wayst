#version 120

uniform vec3 clr;

void main() {
    gl_FragData[0] =  vec4(clr, 1);
}

