#include <stdarg.h>

enum fmt_arg_type_e
{
    FMT_ARG_TYPE_INT32,
    FMT_ARG_TYPE_UINT32,
    FMT_ARG_TYPE_FLOAT,
    FMT_ARG_TYPE_DOUBLE,
    FMT_ARG_TYPE_BOOL,
    FMT_ARG_TYPE_STRING,
};

/* #define FMT_GET_ARG_TYPE(x) \ */
/*     __builtin_choose_expr( \ */
/*       __builtin_types_compatible_p(typeof(x), int32_t), \ */
/*       FMT_ARG_TYPE_INT32, \ */
/*       __builtin_choose_expr( \ */
/*         __builtin_types_compatible_p(typeof(x), uint32_t), \ */
/*         FMT_ARG_TYPE_UINT32, \ */
/*         __builtin_choose_expr( \ */
/*           __builtin_types_compatible_p(typeof(x), bool), \ */
/*           FMT_ARG_TYPE_BOOL, \ */
/*           __builtin_choose_expr( \ */
/*             __builtin_types_compatible_p(typeof(x), float), \ */
/*             FMT_ARG_TYPE_FLOAT, \ */
/*             __builtin_choose_expr( \ */
/*               __builtin_types_compatible_p(typeof(x), double), \ */
/*               FMT_ARG_TYPE_DOUBLE, \ */
/*               __builtin_choose_expr(__builtin_types_compatible_p(typeof(x), char*), \ */
/*                                     FMT_ARG_TYPE_STRING, \ */
/*                                     /\* throw error *\/ (void)0)))))) */
/* #define FMT_ARG(_arg) \ */
/*     (fmt_arg_t) { .name = #_arg, .type = FMT_GET_ARG_TYPE(_arg), .addr = &(_arg), } */

#define FMT_ARG_I32(_arg)                                                                          \
    (fmt_arg_t) { .name = #_arg, .type = FMT_ARG_TYPE_INT32, .addr = &(_arg), }

#define FMT_ARG_U32(_arg)                                                                          \
    (fmt_arg_t) { .name = #_arg, .type = FMT_ARG_TYPE_UINT32, .addr = &(_arg), }

#define FMT_ARG_F32(_arg)                                                                          \
    (fmt_arg_t) { .name = #_arg, .type = FMT_ARG_TYPE_FLOAT, .addr = &(_arg), }

#define FMT_ARG_F64(_arg)                                                                          \
    (fmt_arg_t) { .name = #_arg, .type = FMT_ARG_TYPE_DOUBLE, .addr = &(_arg), }

#define FMT_ARG_BOOL(_arg)                                                                         \
    (fmt_arg_t) { .name = #_arg, .type = FMT_ARG_TYPE_BOOL, .addr = &(_arg), }

#define FMT_ARG_STR(_arg)                                                                          \
    (fmt_arg_t) { .name = #_arg, .type = FMT_ARG_TYPE_STRING, .addr = &(_arg), }

typedef struct
{
    const char*         name;
    enum fmt_arg_type_e type;
    void*               addr;
} fmt_arg_t;

__attribute__((sentinel)) char* fmt_new_interpolated(const char* formatter,
                                                     char**      opt_out_err,
                                                     fmt_arg_t*,
                                                     ...);
