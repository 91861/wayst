/* This file was autogenerated from shaders_gles20. */

const char*
font_vs_src =
"#version 100\n"
"precision mediump float;"
"attribute vec4 coord;"
"varying vec2 tex_coord;"
"void main(){"
"tex_coord=coord.zw;"
"gl_Position=vec4(coord.xy,0,1);"
"}";


const char*
image_rgb_vs_src =
"#version 100\n"
"precision mediump float;"
"attribute vec4 coord;"
"uniform vec2 offset;"
"varying vec2 tex_coord;"
"void main(){"
"tex_coord=coord.zw;"
"gl_Position=vec4(coord.xy+offset,0,1);"
"}";


const char*
line_vs_src =
"#version 100\n"
"precision mediump float;"
"attribute vec2 pos;"
"void main(){"
"gl_Position=vec4(pos,0,1);"
"}";


const char*
solid_fill_vs_src =
"#version 100\n"
"precision mediump float;"
"attribute vec2 pos;"
"void main(){"
"gl_Position=vec4(pos,0,1);"
"}";


const char*
font_depth_blend_fs_src =
"#version 100\n"
"precision mediump float;"
"uniform sampler2D tex;"
"varying vec2 tex_coord;"
"void main(){"
"vec3 c=texture2D(tex,tex_coord).rgb;"
"float a=length(c)/length(vec3(1,1,1));"
"gl_FragColor=vec4(c,a);"
"}";


const char*
font_fs_src =
"#version 100\n"
"precision mediump float;"
"uniform vec3 clr;"
"uniform vec4 bclr;"
"uniform sampler2D tex;"
"varying vec2 tex_coord;"
"void main()"
"{"
"vec3 c=texture2D(tex,tex_coord).rgb;"
"float a=1.0-length(c)/length(vec3(1,1,1));"
"gl_FragColor=vec4(mix(bclr.r,clr.r,c.r),"
"mix(bclr.g,clr.g,c.g),"
"mix(bclr.b,clr.b,c.b),"
"a*a);"
"}";


const char*
font_gray_fs_src =
"#version 100\n"
"precision mediump float;"
"uniform vec3 clr;"
"uniform sampler2D tex;"
"varying vec2 tex_coord;"
"void main(){"
"vec3 c=texture2D(tex,tex_coord).rgb;"
"gl_FragColor=vec4(clr.r*c.r,"
"clr.g*c.r,"
"clr.b*c.r,"
"(1.0-c.r)*(1.0-c.r));"
"}";


const char*
image_rgb_fs_src =
"#version 100\n"
"precision mediump float;"
"uniform sampler2D tex;"
"varying vec2 tex_coord;"
"void main(){"
"gl_FragColor=texture2D(tex,tex_coord);"
"}";


const char*
image_tint_rgb_fs_src =
"#version 100\n"
"precision mediump float;"
"uniform sampler2D tex;"
"uniform vec3 tint;"
"varying vec2 tex_coord;"
"void main(){"
"gl_FragData[0]=texture2D(tex,tex_coord)*vec4(tint,1.0);"
"}";


const char*
line_fs_src =
"#version 100\n"
"precision mediump float;"
"uniform vec3 clr;"
"void main(){"
"gl_FragColor=vec4(clr,1);"
"}";


const char*
solid_fill_fs_src =
"#version 100\n"
"precision mediump float;"
"uniform vec4 clr;"
"void main(){"
"gl_FragColor=clr;"
"}";
