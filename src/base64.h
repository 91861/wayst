#pragma once

#include <stddef.h>

ptrdiff_t base64_decode(const char* input, char* output);
void      base64_encode(const char* input, size_t size, char* output);
__attribute__((warn_unused_result)) char* base64_encode_alloc(const char* input,
                                                              size_t      size,
                                                              size_t*     opt_out_size);
