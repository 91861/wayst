/* See LICENSE for license information. */

#pragma once

#define _GNU_SOURCE

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TERMCOLOR_RESET "\e[m"

#define TERMCOLOR_DEFAULT       "\e[39m"
#define TERMCOLOR_BLACK         "\e[30m"
#define TERMCOLOR_RED           "\e[31m"
#define TERMCOLOR_GREEN         "\e[32m"
#define TERMCOLOR_YELLOW        "\e[33m"
#define TERMCOLOR_BLUE          "\e[34m"
#define TERMCOLOR_MAGENTA       "\e[35m"
#define TERMCOLOR_CYAN          "\e[36m"
#define TERMCOLOR_GRAY          "\e[37m"
#define TERMCOLOR_GRAY_DARK     "\e[90m"
#define TERMCOLOR_RED_LIGHT     "\e[91m"
#define TERMCOLOR_GREEN_LIGHT   "\e[92m"
#define TERMCOLOR_YELLOW_LIGHT  "\e[93m"
#define TERMCOLOR_BLUE_LIGHT    "\e[94m"
#define TERMCOLOR_MAGENTA_LIGHT "\e[95m"
#define TERMCOLOR_CYAN_LIGHT    "\e[96m"
#define TERMCOLOR_WHITE         "\e[97m"

#define TERMCOLOR_BG_DEFAULT       "\e[49m"
#define TERMCOLOR_BG_BLACK         "\e[40m"
#define TERMCOLOR_BG_RED           "\e[41m"
#define TERMCOLOR_BG_GREEN         "\e[42m"
#define TERMCOLOR_BG_YELLOW        "\e[43m"
#define TERMCOLOR_BG_BLUE          "\e[44m"
#define TERMCOLOR_BG_MAGENTA       "\e[45m"
#define TERMCOLOR_BG_CYAN          "\e[46m"
#define TERMCOLOR_BG_GRAY          "\e[47m"
#define TERMCOLOR_BG_GRAY_DARK     "\e[100m"
#define TERMCOLOR_BG_RED_LIGHT     "\e[101m"
#define TERMCOLOR_BG_GREEN_LIGHT   "\e[102m"
#define TERMCOLOR_BG_YELLOW_LIGHT  "\e[103m"
#define TERMCOLOR_BG_BLUE_LIGHT    "\e[104m"
#define TERMCOLOR_BG_MAGENTA_LIGHT "\e[105m"
#define TERMCOLOR_BG_CYAN_LIGHT    "\e[106m"
#define TERMCOLOR_BG_WHITE         "\e[107m"

#define likely(_x)     __builtin_expect((bool)(_x), 1)
#define unlikely(_x)   __builtin_expect((bool)(_x), 0)
#define expect(_x, _y) __builtin_expect((_x), (_y))

#define POW2(_x)            ((_x) * (_x))
#define POW3(_x)            ((_x) * (_x) * (_X))
#define MAX(_a, _b)         ((_a) > (_b) ? (_a) : (_b))
#define MIN(_a, _b)         ((_a) < (_b) ? (_a) : (_b))
#define CLAMP(_v, _lo, _hi) ((_v) > (_hi) ? (_hi) : (_v) < (_lo) ? (_lo) : (_v))

#define ARRAY_SIZE(_array) (sizeof((_array)) / sizeof((_array[0])))
#define ARRAY_LAST(_array) (_array[(sizeof((_array)) / sizeof((_array[0]))) -1])

#define STATIC_ASSERT(cond, msg)                                               \
    typedef char static_assertion_##msg[(cond) ? 1 : -1]

#define WRN(...)                                                               \
    {                                                                          \
        fputs("\e[33m", stderr);                                               \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputs("\e[m", stderr);                                                 \
    }

#ifdef DEBUG
#define ERR(...)                                                               \
    {                                                                          \
        fputs("\e[31m", stderr);                                               \
        fprintf(stderr, __VA_ARGS__);                                          \
        fprintf(stderr, "\nIn file: \"%s\" function: \"%s\" line: %d\n",       \
                __FILE__, __func__, __LINE__);                                 \
        fputs("\e[m", stderr);                                                 \
        exit(EXIT_FAILURE);                                                    \
    }
#define ASSERT(cond, msg)                                                      \
    {                                                                          \
        if ((cond) == false) {                                                 \
            ERR("Assertion failed: \'%s\', %s", #cond, msg);                   \
        }                                                                      \
    }
#define ASSERT_UNREACHABLE                                                     \
    {                                                                          \
        ERR("got to section declared unreachable. file: %s func: %s line: %d", \
            __FILE__, __func__, __LINE__);                                     \
        __builtin_unreachable();                                               \
    }

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define ASSERT(...)                                                            \
    {                                                                          \
        ;                                                                      \
    }
#define ASSERT_UNREACHABLE                                                     \
    {                                                                          \
        __builtin_unreachable();                                               \
    }
#define LOG(...)                                                               \
    {                                                                          \
        ;                                                                      \
    }
#define ERR(...)                                                               \
    {                                                                          \
        fputs("\e[31m", stderr);                                               \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputs("\e[m\n", stderr);                                               \
        exit(EXIT_FAILURE);                                                    \
    }
#endif

#ifndef asprintf
#define asprintf(...) _asprintf(__VA_ARGS__)
#endif

static inline char* _asprintf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char* buf = malloc(1 + vsnprintf(NULL, 0, fmt, ap));
    va_end(ap);
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);

    return buf;
}

__attribute__((always_inline)) static inline void* smalloc(size_t size)
{
    void* res = malloc(size);
    if (unlikely(!res)) {
        usleep(1);
        if (!(res = malloc(size))) {
            usleep(5);
            if (!(res = malloc(size)))
                ERR("bad alloc");
        }
    }

    return res;
}

__attribute__((always_inline)) static inline void* scalloc(size_t nmemb,
                                                           size_t size)
{
    void* res = calloc(nmemb, size);
    if (unlikely(!res)) {
        usleep(1);
        if (!(res = calloc(nmemb, size))) {
            usleep(5);
            if (!(res = calloc(nmemb, size)))
                ERR("bad alloc");
        }
    }

    return res;
}

__attribute__((always_inline)) static inline void* srealloc(void*  ptr,
                                                            size_t size)
{
    void* res = realloc(ptr, size);
    if (unlikely(!res)) {
        usleep(1);
        if (!(res = realloc(ptr, size))) {
            usleep(5);
            if (!(res = realloc(ptr, size)))
                ERR("bad alloc");
        }
    }

    return res;
}

#define FLAG_SET(fld, flg)    (fld |= (flg))
#define FLAG_UNSET(fld, flg)  (fld &= ~(flg))
#define FLAG_FLIP(fld, flg)   (fld ^= (flg))
#define FLAG_IS_SET(fld, flg) (fld & (flg))
#define BIT_SET(fld, bit)     (fld |= (1 << bit))
#define BIT_UNSET(fld, bit)   (fld &= ~(1 << bit))
#define BIT_FLIP(fld, bit)    (fld ^= (1 << bit))
#define BIT_IS_SET(fld, bit)  (fld & (1 << bit))

#define BIN_8_FMT "%c%c%c%c%c%c%c%c"
#define BIN_8_AP(byte)                                                         \
    (byte & 0b10000000 ? '1' : '0'), (byte & 0b01000000 ? '1' : '0'),          \
      (byte & 0b00100000 ? '1' : '0'), (byte & 0b00010000 ? '1' : '0'),        \
      (byte & 0b00001000 ? '1' : '0'), (byte & 0b00000100 ? '1' : '0'),        \
      (byte & 0b00000010 ? '1' : '0'), (byte & 0b00000001 ? '1' : '0')

#define BIN_16_FMT BIN_8_FMT " " BIN_8_FMT
#define BIN_16_AP(byte)                                                        \
    (byte & 0b1000000000000000 ? '1' : '0'),                                   \
      (byte & 0b0100000000000000 ? '1' : '0'),                                 \
      (byte & 0b0010000000000000 ? '1' : '0'),                                 \
      (byte & 0b0001000000000000 ? '1' : '0'),                                 \
      (byte & 0b0000100000000000 ? '1' : '0'),                                 \
      (byte & 0b0000010000000000 ? '1' : '0'),                                 \
      (byte & 0b0000001000000000 ? '1' : '0'),                                 \
      (byte & 0b0000000100000000 ? '1' : '0'),                                 \
      (byte & 0b0000000010000000 ? '1' : '0'),                                 \
      (byte & 0b0000000001000000 ? '1' : '0'),                                 \
      (byte & 0b0000000000100000 ? '1' : '0'),                                 \
      (byte & 0b0000000000010000 ? '1' : '0'),                                 \
      (byte & 0b0000000000001000 ? '1' : '0'),                                 \
      (byte & 0b0000000000000100 ? '1' : '0'),                                 \
      (byte & 0b0000000000000010 ? '1' : '0'),                                 \
      (byte & 0b0000000000000001 ? '1' : '0')

#define BIN_32_FMT BIN_16_FMT " " BIN_16_FMT
#define BIN_32_AP(byte)                                                        \
    (byte & 0b10000000000000000000000000000000 ? '1' : '0'),                   \
      (byte & 0b01000000000000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00100000000000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00010000000000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00001000000000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000100000000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000010000000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000001000000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000100000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000010000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000001000000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000100000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000010000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000001000000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000100000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000010000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000001000000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000100000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000010000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000001000000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000100000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000010000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000001000000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000000100000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000000010000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000000001000000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000000000100000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000000000010000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000000000001000 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000000000000100 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000000000000010 ? '1' : '0'),                 \
      (byte & 0b00000000000000000000000000000001 ? '1' : '0')

/* Pair struct */
#define DEF_PAIR(a)                                                            \
    typedef struct                                                             \
    {                                                                          \
        a first;                                                               \
        a second;                                                              \
    } Pair_##a

DEF_PAIR(uint8_t);
DEF_PAIR(uint16_t);
DEF_PAIR(uint32_t);
DEF_PAIR(uint64_t);
DEF_PAIR(int8_t);
DEF_PAIR(int16_t);
DEF_PAIR(int32_t);
DEF_PAIR(int64_t);
DEF_PAIR(char);
DEF_PAIR(int);
DEF_PAIR(unsigned);
DEF_PAIR(short);
DEF_PAIR(long);
DEF_PAIR(float);
DEF_PAIR(double);
DEF_PAIR(wchar_t);
DEF_PAIR(size_t);

/** check string equality case insensitive */
__attribute__((always_inline)) static inline bool strneqci(const char*  s1,
                                                           const char*  s2,
                                                           const size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (tolower(s1[i]) != tolower(s2[i]))
            return false;
    return true;
}

/** match string against wildcard pattern */
static bool streq_wildcard(const char* str, const char* pattern)
{
    while (*pattern && *str)
        switch (*pattern) {
            default:
                if (*pattern != *str)
                    return false;
                /* fall through */
            case '?':
                ++pattern;
                ++str;
                break;
            case '*':
                if (pattern[1] == '\0' || pattern[1] == '*')
                    return true;
                ++pattern;
                while (*pattern && *str)
                    if (streq_wildcard(str++, pattern))
                        return true;
                return false;
        }

    size_t num_stars = 0;
    for (const char* i = pattern; *i; ++i)
        if (*i == '*')
            ++num_stars;
    return strlen(str) == (strlen(pattern) - num_stars);
}

/** convert string to bool (false if fails) */
__attribute__((always_inline, flatten)) static inline bool strtob(
  const char* str)
{
    if (!str)
        return false;
    while (!isalnum(*str++))
        ;
    return strneqci("true", str, 4) || strneqci("1", str, 1);
}

/* UTF-8 decoding and encoding */
static const uint8_t utf_mask[] = { 0b00000000, 0b10000000, 0b11000000,
                                    0b11100000, 0b11110000, 0b11111000 };

__attribute__((always_inline)) static inline uint32_t utf8_seq_len(const char c)
{
    uint32_t seq_len = 1;
    if ((c & utf_mask[1]) != utf_mask[0]) {
        for (seq_len = 2; seq_len <= 4; ++seq_len)
            if ((c & utf_mask[seq_len + 1]) == utf_mask[seq_len])
                goto good;
        return 0; /* sequence invalid */
    good:;
    }
    return seq_len;
}

/**
 * Decode UTF-8 character
 *
 * @param limit last character that can be read (checks if sequence is complete)
 * NULL - do not check
 *
 * @return NULL if sequence incomplete
 */
__attribute__((always_inline)) static inline uint32_t utf8_decode(
  const char* s,
  const char* limit)
{
    uint32_t len = utf8_seq_len(*s);
    if (limit && (limit - s) <= len)
        return 0;
    uint32_t res = 0;
    switch (len) {
        case 1:
            return *s;
        case 2:
            res = *s & 0b00111111;
            break;
        case 3:
            res = *s & 0b00001111;
            break;
        case 4:
            res = *s & 0b00000111;
            break;
        default:
            return 0;
    }
    for (uint32_t i = 1; i < len; ++i)
        res = (res << 6) | (*++s & 0b00111111);
    return res;
}

/** assumes valid input */
__attribute__((always_inline)) static inline uint32_t utf8_len(uint32_t code)
{
    //                              2^7  2^11  2^16   2^21
    static const uint32_t max[] = { 128, 2048, 65536, 2097152 };
    for (uint32_t i = 0; i < 4; ++i)
        if (code < max[i])
            return i + 1;
    return 0;
}

/**
 * @return bytes written (sequence length), 0 on failure */
__attribute__((always_inline)) static inline uint32_t utf8_encode(uint32_t code,
                                                                  char* output)
{
    uint32_t len = utf8_len(code);

    switch (len) {
        case 1:
            *output = (char)code;
            break;
        case 2:
            output[1] = 0b10000000 | (0b00111111 & code);
            output[0] = 0b11000000 | (0b00011111 & (code >> 6));
            break;
        case 3:
            output[2] = 0b10000000 | (0b00111111 & code);
            output[1] = 0b10000000 | (0b00111111 & (code >> 6));
            output[0] = 0b11100000 | (0b00001111 & (code >> 6));
            break;
        case 4:
            output[3] = 0b10000000 | (0b00111111 & code);
            output[2] = 0b10000000 | (0b00111111 & (code >> 6));
            output[1] = 0b10000000 | (0b00111111 & (code >> 6));
            output[0] = 0b11110000 | (0b00000111 & (code >> 6));
    }
    return len;
}
