

#ifndef ALLIMGFMTS

#define STBI_NO_JPEG
#define STBI_NO_GIF
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#define STBI_NO_TGA

#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image/stb_image.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image/stb_image_write.h"
