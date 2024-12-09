#include "config_parser.h"

#include <ctype.h>
#include <stdbool.h>

static inline bool call_on_syntax_error_helper(
  bool (*on_syntax_error_func)(uint32_t line, const char* message_format, va_list format_args),
  uint32_t    line,
  const char* fmt_string,
  ...)
{
    va_list ap;
    va_start(ap, fmt_string);
    bool retval = on_syntax_error_func(line, fmt_string, ap);
    va_end(ap);
    return retval;
}

void settings_file_parse(
  FILE* f,
  void (*on_property_read_func)(const char* key, const char* value, uint32_t line),
  bool (*opt_on_syntax_error_func)(uint32_t line, const char* message_format, va_list format_args))
{
    ASSERT(f, "valid FILE*");
    ASSERT(on_property_read_func, "has callback");

    char buf[1024 * 8] = { 0 };
    int  rd;

    Vector_char key        = Vector_new_with_capacity_char(10);
    Vector_char value      = Vector_new_with_capacity_char(30);
    Vector_char whitespace = Vector_new_char();

    bool in_list = false, in_comment = false, in_value = false, in_string = false, escaped = false;

    uint32_t key_line = 0, line = 1;

    while ((rd = fread(buf, sizeof(char), sizeof buf - 1, f))) {
        for (int i = 0; i < rd; ++i) {
            char c = buf[i];

            if (c == '\n') {
                ++line;
            } else if (c == '#' && !escaped && !in_string) {
                in_comment = true;
                continue;
            }

            if (in_comment) {
                if (c == '\n') {
                    in_comment = false;
                }
            }

            if (in_value) {
                if (c == '\\' && !escaped) {
                    escaped = true;
                    if (in_list) {
                        Vector_push_char(&value, c);
                    }
                    continue;
                } else if (c == '\"' && !escaped) {
                    if (!in_string && value.size && !in_list) {
                        Vector_push_char(&value, '\0');

                        if (opt_on_syntax_error_func &&
                            call_on_syntax_error_helper(opt_on_syntax_error_func,
                                                        line,
                                                        "unexpected token \'%s\' before \'\"\'",
                                                        value.buf)) {
                            goto abort;
                        }

                        Vector_clear_char(&value);
                    }

                    if (!in_string) {
                        Vector_clear_char(&whitespace);
                    }

                    in_string = !in_string;

                    if (in_list) {
                        Vector_push_char(&value, c);
                    }

                    continue;
                } else if (c == '[' && !escaped && !in_string) {
                    if (in_list) {
                        if (opt_on_syntax_error_func &&
                            call_on_syntax_error_helper(
                              opt_on_syntax_error_func,
                              line,
                              "list element cannot be a list, did you mean \'\\[\' ?")) {
                            goto abort;
                        }
                    }
                    in_list = true;
                } else if (c == ']' && !in_string && !escaped) {
                    if (!in_list) {
                        if (opt_on_syntax_error_func &&
                            call_on_syntax_error_helper(
                              opt_on_syntax_error_func,
                              line,
                              "\'[\' expected before \']\' did you mean \'\\]\' ?")) {
                            goto abort;
                        }
                    }
                    in_list = false;
                }

                if (c == '\n' && !in_list) {
                    if (in_value || key.size) {
                        Vector_push_char(&value, '\0');
                        on_property_read_func(key.buf, value.buf, key_line);
                        if (in_string) {
                            if (opt_on_syntax_error_func &&
                                call_on_syntax_error_helper(opt_on_syntax_error_func,
                                                            line,
                                                            "\'\"\' expected before end of line")) {
                                goto abort;
                            }
                        }
                    }
                    Vector_clear_char(&whitespace);
                    Vector_clear_char(&key);
                    Vector_clear_char(&value);
                    in_value  = false;
                    in_string = false;
                    continue;
                } else if (escaped && !in_list) {
                    if (c == 'n') {
                        Vector_push_char(&value, '\n');
                    } else if (c == '\"' && in_string) {
                        Vector_push_char(&value, '\"');
                    } else {
                        if (opt_on_syntax_error_func &&
                            call_on_syntax_error_helper(
                              opt_on_syntax_error_func,
                              line,
                              "escape character \'%c\' invalid in this context",
                              c)) {
                            goto abort;
                        }
                    }
                } else if (!iscntrl(c) && !in_comment) {
                    switch (c) {
                        case ']':
                        case '[':
                            if (in_string) {
                                Vector_push_char(&value, '\\');
                            }
                    }
                    if (in_string || !isblank(c)) {
                        if (whitespace.size) {
                            Vector_pushv_char(&value, whitespace.buf, whitespace.size);
                            Vector_clear_char(&whitespace);
                        }
                        Vector_push_char(&value, c);
                    } else if (value.size) {
                        Vector_push_char(&whitespace, c);
                    }
                }
                escaped = false;
            }
            /* in key */
            else if (c == '=') {
                if (!in_comment) {
                    Vector_push_char(&key, '\0');
                    key_line = line;
                    in_value = true;
                }
            } else if (c == '\n' && key.size) {
                if (!in_comment) {
                    Vector_push_char(&key, '\0');
                    on_property_read_func(key.buf, NULL, key_line);
                    Vector_clear_char(&key);
                }
            } else {
                if (!in_comment) {
                    if (!iscntrl(c) && !isblank(c)) {
                        Vector_push_char(&key, c);
                    }
                }
            }
        }
    }

    if (key.size) {
        Vector_push_char(&key, '\0');
        Vector_push_char(&value, '\0');
        on_property_read_func(key.buf, value.size > 1 ? value.buf : NULL, line);
    }

    if (in_string) {

        if (opt_on_syntax_error_func &&
            call_on_syntax_error_helper(opt_on_syntax_error_func,
                                        line,
                                        "\'\"\' expected before end of file")) {
            goto abort;
        }
    }
    if (in_list) {
        if (opt_on_syntax_error_func) {
            call_on_syntax_error_helper(opt_on_syntax_error_func,
                                        line,
                                        "\']\' expected before end of file");
        }
    }

abort:
    Vector_destroy_char(&key);
    Vector_destroy_char(&value);
    Vector_destroy_char(&whitespace);
}

static inline void call_on_syntax_error_helper2(
  void (*on_syntax_error_func)(const char* message_format, va_list format_args),
  const char* fmt_string,
  ...)
{
    va_list ap;
    va_start(ap, fmt_string);
    on_syntax_error_func(fmt_string, ap);
    va_end(ap);
}

Vector_Vector_char expand_list_value(const char* const list,
                                     void (*opt_on_syntax_error_func)(const char* message_format,
                                                                      va_list     format_args))
{
    Vector_Vector_char values = Vector_new_with_capacity_Vector_char(1);
    Vector_push_Vector_char(&values, Vector_new_with_capacity_char(10));

    if (!list) {
        Vector_push_char(Vector_last_Vector_char(&values), '\0');
        return values;
    }

    bool in_string    = false;
    bool in_list      = false;
    bool escaped      = false;
    bool has_brackets = false;
    bool not_a_list   = true;

    for (const char* i = list; *i; ++i) {
        if (*i == '[' && !escaped) {
            not_a_list = false;
            break;
        } else if (!(escaped = (*i == '\\' && !escaped))) {
            Vector_push_char(Vector_last_Vector_char(&values), *i);
        }
    }
    escaped = false;

    if (not_a_list) {
        Vector_push_char(Vector_last_Vector_char(&values), '\0');
        return values;
    }

    Vector_char whitespace = Vector_new_char();
    const char* arr        = list;
    for (char c = *arr; c; c = *(++arr)) {
        if (!escaped && !in_string && c == '[') {
            has_brackets = true;
            in_list      = true;
            escaped      = false;
            continue;
        }
        if (!escaped && !in_string && in_list && c == ']') {
            in_list = false;
            escaped = false;
            continue;
        }
        if (!escaped && c == '\"') {
            if (!in_string) {
                Vector_clear_char(&whitespace);
            }
            in_string = !in_string;
            continue;
        }
        if (!escaped && c == '\\') {
            escaped = true;
            continue;
        }
        if (!in_string && !escaped && c == ',') {
            Vector_push_char(Vector_last_Vector_char(&values), '\0');
            Vector_push_Vector_char(&values, Vector_new_with_capacity_char(10));
            Vector_clear_char(&whitespace);
            escaped = false;
            continue;
        }
        escaped = false;
        if (in_string || !isblank(c)) {
            if (isblank(c) && Vector_last_Vector_char(&values)->size) {
                /* May be whitespace inside the string, hold on to this and insert before the next
                 * character if we get one */
                Vector_push_char(&whitespace, c);
            } else {
                if (whitespace.size) {
                    Vector_pushv_char(Vector_last_Vector_char(&values),
                                      whitespace.buf,
                                      whitespace.size);
                }
                Vector_clear_char(&whitespace);
                Vector_push_char(Vector_last_Vector_char(&values), c);
            }
        }
    }
    Vector_push_char(Vector_last_Vector_char(&values), '\0');

    if (in_list) {
        if (opt_on_syntax_error_func) {
            call_on_syntax_error_helper2(opt_on_syntax_error_func,
                                         "list not terminated in \'%s\'",
                                         list);
        }
    } else if (values.size == 1 && has_brackets) {
        if (opt_on_syntax_error_func) {
            call_on_syntax_error_helper2(
              opt_on_syntax_error_func,
              "\'%s\' is a single element list, did you mean \'\\[%s\\]\'?",
              list,
              values.buf[0].buf);
        }
    }
    if (in_string) {
        if (opt_on_syntax_error_func) {
            call_on_syntax_error_helper2(opt_on_syntax_error_func,
                                         "string not terminated in linst \'%s\'",
                                         list);
        }
    }
    Vector_destroy_char(&whitespace);

    return values;
}
