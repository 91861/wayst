#define _GNU_SOURCE

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "fmt.h"

#include "map.h"
#include "vector.h"

static inline size_t _str_as_size_t_hash(const size_t* val)
{
    size_t acc = 0;
    for (char* c = *((char**)val); *c; c++)
        acc += *c;
    return acc;
}

static inline bool _str_as_size_t_eq(const size_t* a, const size_t* b)
{
    return !strcmp(*((char**)a), *((char**)b));
}

DEF_MAP(size_t, fmt_arg_t, _str_as_size_t_hash, _str_as_size_t_eq, NULL);

DEF_VECTOR(char, NULL);
DEF_VECTOR(Vector_char, Vector_destroy_char);

static char* fmt_to_string(const fmt_arg_t* var)
{
    switch (var->type) {
        case FMT_ARG_TYPE_BOOL:
            return asprintf(BOOL_FMT, BOOL_AP(*((bool*)var->addr)));
        case FMT_ARG_TYPE_STRING:
            return asprintf("%s", *((char**)var->addr));
        case FMT_ARG_TYPE_INT32:
            return asprintf("%d", *((int32_t*)var->addr));
        case FMT_ARG_TYPE_UINT32:
            return asprintf("%u", *((uint32_t*)var->addr));
        case FMT_ARG_TYPE_DOUBLE:
            return asprintf("%f", *((double*)var->addr));
        case FMT_ARG_TYPE_FLOAT:
            return asprintf("%f", *((float*)var->addr));
        default:
            ASSERT_UNREACHABLE;
    }
}

static void drop_whitespace(Vector_char* expr)
{
    char* p = NULL;
    while ((p = Vector_first_char(expr)) && isblank(*p)) {
        Vector_remove_at_char(expr, 0, 1);
    }
    p = NULL;
    while ((p = Vector_last_char(expr)) && (isblank(*p) || !*p)) {
        Vector_pop_char(expr);
    }
    Vector_push_char(expr, '\0');
}

static double _fmt_eval_operand(Map_size_t_fmt_arg_t* vars, Vector_char* expr, char** e)
{
    bool negate = false;
    drop_whitespace(expr);
    if (*Vector_first_char(expr) == '!') {
        Vector_remove_at_char(expr, 0, 1);
        negate = true;
    }
    if (!strcasecmp(expr->buf, "true")) {
        return !negate;
    }
    if (!strcasecmp(expr->buf, "false")) {
        return negate;
    }

    size_t     k   = (size_t)expr->buf;
    fmt_arg_t* arg = Map_get_size_t_fmt_arg_t(vars, &k);

    if (arg) {
        switch (arg->type) {
            case FMT_ARG_TYPE_BOOL:
                return negate ? !*((bool*)arg->addr) : *((bool*)arg->addr);
            case FMT_ARG_TYPE_INT32: {
                int32_t v = *((int32_t*)arg->addr);
                if (negate) {
                    return v == 0.0 ? 1.0 : 0.0;
                } else {
                    return v;
                }
            }
            case FMT_ARG_TYPE_UINT32: {
                uint32_t v = *((uint32_t*)arg->addr);
                if (negate) {
                    return v == 0.0 ? 1.0 : 0.0;
                } else {
                    return v;
                }
            }
            case FMT_ARG_TYPE_DOUBLE:
                return *((double*)arg->addr);
            case FMT_ARG_TYPE_FLOAT:
                return *((float*)arg->addr);
            case FMT_ARG_TYPE_STRING:
                *e = "cannot cast string to numeric type";
                return 0;
        }
    } else {
        bool fnd_alpha = false;
        for (char* c = expr->buf; *c; ++c) {
            if (isalpha(*c)) {
                fnd_alpha = true;
                break;
            }
        }

        if (0 && fnd_alpha) {
            *e = "reference to undefined variable";
        } else {
            double v;
            int    r = sscanf(expr->buf, "%lg", &v);
            return (r == 0 || r == EOF) ? 0.0 : v;
        }
    }
    return 0;
}

static bool _fmt_eval_op_cond(Map_size_t_fmt_arg_t* vars, Vector_char* expr, char** e)
{
    char* ee = NULL;
    Vector_push_char(expr, '\0');
    char* rhs = NULL;

    Vector_char lv = Vector_new_char(), rv = Vector_new_char();
    bool        retval = false;

#define L_EVAL_OPS(_len)                                                                           \
    rhs += (_len);                                                                                 \
    Vector_pushv_char(&lv, expr->buf, rhs - expr->buf - (_len));                                   \
    Vector_pushv_char(&rv, rhs, (expr->buf + (expr->size - 1)) - rhs);                             \
    double l = _fmt_eval_operand(vars, &lv, &ee);                                                  \
    if (ee)                                                                                        \
        *e = ee;                                                                                   \
    double r = _fmt_eval_operand(vars, &rv, &ee);                                                  \
    if (ee)                                                                                        \
        *e = ee;

    if ((rhs = strstr(expr->buf, "<="))) {
        L_EVAL_OPS(2);
        retval = l <= r;
    } else if ((rhs = strstr(expr->buf, ">="))) {
        L_EVAL_OPS(2);
        retval = l >= r;
    } else if ((rhs = strstr(expr->buf, "<"))) {
        L_EVAL_OPS(1);
        retval = l < r;
    } else if ((rhs = strstr(expr->buf, ">"))) {
        L_EVAL_OPS(1);
        retval = l > r;
    } else if ((rhs = strstr(expr->buf, "!="))) {
        rhs += 2;
        Vector_pushv_char(&lv, expr->buf, rhs - expr->buf - 2);
        Vector_pushv_char(&rv, rhs, (expr->buf + (expr->size - 1)) - rhs);
        drop_whitespace(&lv);
        drop_whitespace(&rv);
        size_t     lk = (size_t)lv.buf;
        size_t     rk = (size_t)rv.buf;
        fmt_arg_t* la = Map_get_size_t_fmt_arg_t(vars, &lk);
        fmt_arg_t* ra = Map_get_size_t_fmt_arg_t(vars, &rk);
        if (la && ra && la->type == FMT_ARG_TYPE_STRING && ra->type == FMT_ARG_TYPE_STRING) {
            retval = !!strcmp((char*)la->addr, (char*)ra->addr);
        } else {
            double l = _fmt_eval_operand(vars, &lv, &ee);
            if (ee)
                *e = ee;
            double r = _fmt_eval_operand(vars, &rv, &ee);
            if (ee)
                *e = ee;
            retval = l != r;
        }
    } else if ((rhs = strstr(expr->buf, "=="))) {
        rhs += 2;
        Vector_pushv_char(&lv, expr->buf, rhs - expr->buf - 2);
        Vector_pushv_char(&rv, rhs, (expr->buf + (expr->size - 1)) - rhs);
        drop_whitespace(&lv);
        drop_whitespace(&rv);
        size_t     lk = (size_t)lv.buf;
        size_t     rk = (size_t)rv.buf;
        fmt_arg_t* la = Map_get_size_t_fmt_arg_t(vars, &lk);
        fmt_arg_t* ra = Map_get_size_t_fmt_arg_t(vars, &rk);
        if (la && ra && la->type == FMT_ARG_TYPE_STRING && ra->type == FMT_ARG_TYPE_STRING) {
            retval = !strcmp((char*)la->addr, (char*)ra->addr);
        } else {
            double l = _fmt_eval_operand(vars, &lv, &ee);
            if (ee)
                *e = ee;
            double r = _fmt_eval_operand(vars, &rv, &ee);
            if (ee)
                *e = ee;
            retval = l == r;
        }
    } else {
        double ev = _fmt_eval_operand(vars, expr, &ee);
        retval    = ev == 0.0 ? false : true;
    }

    Vector_destroy_char(&lv);
    Vector_destroy_char(&rv);
    return retval;
}

static bool _fmt_eval_conjunction(Map_size_t_fmt_arg_t* vars, Vector_char* expr, char** e)
{
    char*              ee           = NULL;
    Vector_Vector_char disjunctions = Vector_new_Vector_char();
    Vector_push_char(expr, '\0');
    char* tmp = strstr(expr->buf, "||");

    if (!tmp) {
        Vector_char t = Vector_new_with_capacity_char(expr->size);
        Vector_pushv_char(&t, expr->buf, expr->size);
        Vector_push_Vector_char(&disjunctions, t);
    } else {
        Vector_char t = Vector_new_char();
        Vector_pushv_char(&t, expr->buf, tmp - expr->buf);
        Vector_push_Vector_char(&disjunctions, t);
        while ((tmp = strstr(tmp, "||"))) {
            tmp += 2;
            t         = Vector_new_char();
            char* nxt = strstr(tmp, "||");
            Vector_pushv_char(&t, tmp, (nxt ? nxt : expr->buf + expr->size - 1) - tmp);
            Vector_push_Vector_char(&disjunctions, t);
        }
    }

    bool retval = false;
    for (Vector_char* i = NULL; (i = Vector_iter_Vector_char(&disjunctions, i));) {
        if (_fmt_eval_op_cond(vars, i, &ee)) {
            if (ee) {
                *e = ee;
            }
            retval = true;
            break;
        }
    }

    Vector_destroy_Vector_char(&disjunctions);
    return retval;
}

static bool fmt_eval_condition(Map_size_t_fmt_arg_t* vars, Vector_char* expr, char** e)
{
    char*              ee           = NULL;
    Vector_Vector_char conjunctions = Vector_new_Vector_char();
    Vector_push_char(expr, '\0');
    char* tmp = strstr(expr->buf, "&&");

    if (!tmp) {
        Vector_char t = Vector_new_with_capacity_char(expr->size);
        Vector_pushv_char(&t, expr->buf, expr->size);
        Vector_push_Vector_char(&conjunctions, t);
    } else {
        Vector_char t = Vector_new_char();
        Vector_pushv_char(&t, expr->buf, tmp - expr->buf);
        Vector_push_Vector_char(&conjunctions, t);
        while ((tmp = strstr(tmp, "&&"))) {
            tmp += 2;
            t         = Vector_new_char();
            char* nxt = strstr(tmp, "&&");
            Vector_pushv_char(&t, tmp, (nxt ? nxt : expr->buf + expr->size - 1) - tmp);
            Vector_push_Vector_char(&conjunctions, t);
        }
    }

    bool retval = true;
    for (Vector_char* i = NULL; (i = Vector_iter_Vector_char(&conjunctions, i));) {
        if (!_fmt_eval_conjunction(vars, i, &ee)) {
            if (ee) {
                *e = ee;
            }
            retval = false;
            break;
        }
    }

    Vector_destroy_Vector_char(&conjunctions);
    return retval;
}

static Vector_char fmt_eval(Map_size_t_fmt_arg_t* vars, Vector_char* expr, char** e)
{
    char*       ee           = NULL;
    Vector_char buf          = Vector_new_with_capacity_char(32);
    Vector_char buf2         = Vector_new_with_capacity_char(32);
    Vector_char ex           = Vector_new_with_capacity_char(32);
    int32_t     expr_lvl     = 0;
    bool        initial_char = false, is_conditional = false, condition_complete = false;

    for (char* c = NULL; (c = Vector_iter_char(expr, c));) {
        if (!initial_char && *c == '?') {
            is_conditional = true;
            continue;
        }

        if (!initial_char && !isblank(*c)) {
            initial_char = true;
        }

        if (is_conditional) {
            if (*c == ':' && expr_lvl == 0) {
                if (!fmt_eval_condition(vars, &buf2, &ee)) {
                    if (ee) {
                        *e = ee;
                    }
                    Vector_clear_char(&buf);
                    break;
                }
                condition_complete = true;
            } else {
                if (condition_complete) {
                    if (expr_lvl == 1) {
                        if (*c == '}') {
                            Vector_char ef = fmt_eval(vars, &ex, &ee);
                            if (ee) {
                                *e = ee;
                            }
                            Vector_pushv_char(&buf, ef.buf, ef.size);
                            Vector_destroy_char(&ef);
                            --expr_lvl;
                        } else {
                            if (*c == '{') {
                                ++expr_lvl;
                            }
                            Vector_push_char(&ex, *c);
                        }
                    } else if (expr_lvl == 0) {
                        if (*c == '{') {
                            Vector_clear_char(&ex);
                            ++expr_lvl;
                        } else
                            Vector_push_char(&buf, *c);
                    } else {
                        if (*c == '}') {
                            --expr_lvl;
                        } else if (*c == '{')
                            ++expr_lvl;
                        Vector_push_char(expr_lvl > 0 ? &ex : &buf, *c);
                    }
                } else {
                    Vector_push_char(&buf2, *c);
                }
            }
        } else {
            Vector_push_char(&buf, *c);
        }
    }

    if (!is_conditional) {
        if (buf.size > 0) {
            Vector_push_char(&buf, '\0');
            size_t           k   = (size_t)buf.buf;
            const fmt_arg_t* var = Map_get_size_t_fmt_arg_t(vars, &k);
            Vector_clear_char(&buf);

            if (var) {
                char* strrepr = fmt_to_string(var);
                Vector_pushv_char(&buf, strrepr, strlen(strrepr));
                free(strrepr);
            } else {
                *e = "reference to undefined variable in interpolated value";
            }
        }
    }

    Vector_destroy_char(&ex);
    Vector_destroy_char(&buf2);
    return buf;
}

__attribute__((sentinel)) char* fmt_new_interpolated(const char* fmt,
                                                     char**      err,
                                                     fmt_arg_t*  arg,
                                                     ...)
{
    char*                e    = NULL;
    Map_size_t_fmt_arg_t vars = Map_new_size_t_fmt_arg_t(32);
    {
        Map_insert_size_t_fmt_arg_t(&vars, (size_t)arg->name, *arg);
        va_list ap;
        va_start(ap, arg);
        fmt_arg_t* a = NULL;
        do {
            if ((a = va_arg(ap, fmt_arg_t*))) {
                Map_insert_size_t_fmt_arg_t(&vars, (size_t)a->name, *a);
            }
        } while (a);
        va_end(ap);
    }

    Vector_char buf = Vector_new_with_capacity_char(32);
    {
        int32_t     expr_lvl = 0;
        bool        escaped  = false;
        Vector_char expr     = Vector_new_with_capacity_char(32);

        for (const char* c = fmt; *c; ++c) {
            if (iscntrl(*c))
                continue;
            if (escaped) {
                escaped = false;
                Vector_push_char(expr_lvl > 0 ? &expr : &buf, *c);
            } else {
                if (*c == '\\') {
                    escaped = true;
                    continue;
                }
                if (expr_lvl == 1) {
                    if (*c == '}') {
                        Vector_char ex = fmt_eval(&vars, &expr, &e);
                        Vector_pushv_char(&buf, ex.buf, ex.size);
                        Vector_destroy_char(&ex);
                        --expr_lvl;
                    } else {
                        if (*c == '{') {
                            ++expr_lvl;
                        }
                        Vector_push_char(&expr, *c);
                    }
                } else if (expr_lvl == 0) {
                    if (*c == '{') {
                        Vector_clear_char(&expr);
                        ++expr_lvl;
                    } else {
                        Vector_push_char(&buf, *c);
                    }
                } else {
                    if (*c == '}') {
                        --expr_lvl;
                    } else if (*c == '{') {
                        ++expr_lvl;
                    }
                    Vector_push_char(expr_lvl > 0 ? &expr : &buf, *c);
                }
            }
        }
        Vector_destroy_char(&expr);
        Vector_push_char(&buf, '\0');
        Vector_shrink_char(&buf);
    }

    Map_destroy_size_t_fmt_arg_t(&vars);

    if (err) {
        *err = e;
    }

    return buf.buf;
}
