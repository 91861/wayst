#pragma once

#include <stddef.h>

ptrdiff_t base64_decode(const char* input, char* output);
void      base64_encode(const char* input, size_t size, char* output);
__attribute__((warn_unused_result)) char* base64_encode_alloc(const char* input,
                                                              size_t      size,
                                                              size_t*     opt_out_size);
__attribute__((warn_unused_result)) char* base64_decode_alloc(const char* input,
                                                              size_t      size,
                                                              size_t*     opt_out_size);

static inline size_t base64_encoded_length(size_t input_size)
{
    return ((input_size % 3 ? (input_size += 3 - (input_size % 3)) : input_size) / 3) * 4;
}

static inline size_t base64_decoded_length(const char* input, size_t input_size)
{
    return input_size * 3 / 4
        -(input[input_size - 1] == '=')
        -(input[input_size - 2] == '=')
        -(input[input_size - 3] == '=');
}
