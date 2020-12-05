/* See LICENSE for license information. */

#include "vt.h"

static void        Vt_insert_new_line(Vt* self);
static inline void Vt_move_cursor(Vt* self, uint16_t column, uint16_t rows);

void Vt_output(Vt* self, const char* buf, size_t len);

#define Vt_output_formated(vt, fmt, ...)                                                           \
    char _tmp[64];                                                                                 \
    int  _len = snprintf(_tmp, sizeof(_tmp), fmt, __VA_ARGS__);                                    \
    Vt_output((vt), _tmp, _len);
