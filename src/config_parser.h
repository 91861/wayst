#pragma once

#include <stdarg.h>

#include "util.h"
#include "vector.h"

DEF_VECTOR(char, NULL)

DEF_VECTOR(Vector_char, Vector_destroy_char);

void settings_file_parse(
  FILE* f,
  void (*on_property_read_func)(const char* key, const char* value, uint32_t line),
  bool (*on_syntax_error_func)(uint32_t line, const char* message_format, va_list format_args));

Vector_Vector_char expand_list_value(const char* const list,
                                     void (*opt_on_syntax_error_func)(const char* message_format,
                                                                      va_list     format_args));
