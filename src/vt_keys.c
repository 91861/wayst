/* See LICENSE for license information. */

#define _GNU_SOURCE

#include "key.h"
#include "vt.h"
#include "vt_private.h"

/**
 *  Substitute keypad keys with normal ones */
static uint32_t numpad_key_convert(uint32_t key)
{
    switch (key) {
        case KEY(KP_Add):
            return '+';
        case KEY(KP_Subtract):
            return '-';
        case KEY(KP_Multiply):
            return '*';
        case KEY(KP_Divide):
            return '/';
        case KEY(KP_Equal):
            return '=';
        case KEY(KP_Decimal):
            return '.';
        case KEY(KP_Separator):
            return '.';
        case KEY(KP_Space):
            return ' ';

        case KEY(KP_Up):
            return KEY(Up);
        case KEY(KP_Down):
            return KEY(Down);
        case KEY(KP_Left):
            return KEY(Left);
        case KEY(KP_Right):
            return KEY(Right);

        case KEY(KP_Page_Up):
            return KEY(Page_Up);
        case KEY(KP_Page_Down):
            return KEY(Page_Down);

        case KEY(KP_Insert):
            return KEY(Insert);
        case KEY(KP_Delete):
            return KEY(Delete);
        case KEY(KP_Home):
            return KEY(Home);
        case KEY(KP_End):
            return KEY(End);
        case KEY(KP_Begin):
            return KEY(Begin);
        case KEY(KP_Tab):
            return KEY(Tab);
        case KEY(KP_Enter):
            return KEY(Return);

        case KEY(KP_F1):
            return KEY(F1);
        case KEY(KP_F2):
            return KEY(F2);
        case KEY(KP_F3):
            return KEY(F3);
        case KEY(KP_F4):
            return KEY(F4);

        case KEY(KP_0)... KEY(KP_9):
            return '0' + key - KEY(KP_0);
        default:
            return key;
    }
}

/**
 * Respond to key event for unicode input
 * @return keypress was consumed */
static bool Vt_maybe_handle_unicode_input_key(Vt*      self,
                                              uint32_t key,
                                              uint32_t rawkey,
                                              uint32_t mods)
{
    if (self->unicode_input.active) {
        if (key == 13) {
            // Enter
            Vector_push_char(&self->unicode_input.buffer, 0);
            self->unicode_input.active = false;
            char32_t result            = strtol(self->unicode_input.buffer.buf, NULL, 16);
            Vector_clear_char(&self->unicode_input.buffer);
            if (result) {
                LOG("unicode input \'%s\' -> %d\n", self->unicode_input.buffer.buf, result);

                char             tmp[32];
                static mbstate_t mbstate;
                int              mb_len = c32rtomb(tmp, result, &mbstate);
                if (mb_len) {
                    Vt_buffered_output(self, tmp, mb_len);
                }
            } else {
                WRN("Failed to parse \'%s\'\n", self->unicode_input.buffer.buf);
            }
        } else if (key == 27) {
            // Escape
            self->unicode_input.buffer.size = 0;
            self->unicode_input.active      = false;
            CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
        } else if (key == 8) {
            // Backspace
            if (self->unicode_input.buffer.size) {
                Vector_pop_char(&self->unicode_input.buffer);
            } else {
                self->unicode_input.buffer.size = 0;
                self->unicode_input.active      = false;
            }
            CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
        } else if (isxdigit(key)) {
            if (self->unicode_input.buffer.size > 8) {
                CALL_FP(self->callbacks.on_visual_bell, self->callbacks.user_data);
            } else {
                Vector_push_char(&self->unicode_input.buffer, key);
                CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
            }
        } else {
            CALL_FP(self->callbacks.on_visual_bell, self->callbacks.user_data);
        }
        return true;
    }
    return false;
}

/**
 * Respond to key event if it is a function key
 * @return keypress was consumed */
static bool Vt_maybe_handle_function_key(Vt* self, uint32_t key, uint32_t mods)
{
    if (key >= KEY(F1) && key <= KEY(F35)) {
        int f_num = (key + 1) - KEY(F1);
        if (mods) {
            if (f_num < 5) {
                Vt_buffered_output_formated(self, "\e[1;%u%c", mods + 1, f_num + 'O');
            } else if (f_num == 5) {
                Vt_buffered_output_formated(self, "\e[%d;%u~", f_num + 10, mods + 1);
            } else if (f_num < 11) {
                Vt_buffered_output_formated(self, "\e[%d;%u~", f_num + 11, mods + 1);
            } else {
                Vt_buffered_output_formated(self, "\e[%d;%u~", f_num + 12, mods + 1);
            }
        } else {
            if (f_num < 5) {
                Vt_buffered_output_formated(self, "\eO%c", f_num + 'O');
            } else if (f_num == 5) {
                Vt_buffered_output_formated(self, "\e[%d~", f_num + 10);
            } else if (f_num < 11) {
                Vt_buffered_output_formated(self, "\e[%d~", f_num + 11);
            } else {
                Vt_buffered_output_formated(self, "\e[%d~", f_num + 12);
            }
        }
        return true;
    } else /* not f-key */ {
        if (mods) {
            if (key == KEY(Insert)) {
                Vt_buffered_output_formated(self, "\e[2;%u~", mods + 1);
                return true;
            } else if (key == KEY(Delete)) {
                Vt_buffered_output_formated(self, "\e[3;%u~", mods + 1);
                return true;
            } else if (key == KEY(Home)) {
                Vt_buffered_output_formated(self, "\e[1;%u~", mods + 1);
                return true;
            } else if (key == KEY(End)) {
                Vt_buffered_output_formated(self, "\e[4;%u~", mods + 1);
                return true;
            } else if (key == KEY(Page_Up)) {
                Vt_buffered_output_formated(self, "\e[5;%u~", mods + 1);
                return true;
            } else if (key == KEY(Page_Down)) {
                Vt_buffered_output_formated(self, "\e[6;%u~", mods + 1);
                return true;
            }

        } else /* no mods */ {
            if (key == KEY(Insert)) {
                Vt_buffered_output(self, "\e[2~", 4);
                return true;
            } else if (key == KEY(Delete)) {
                Vt_buffered_output(self, "\e[3~", 4);
                return true;
            } else if (key == KEY(Page_Up)) {
                Vt_buffered_output(self, "\e[5~", 4);
                return true;
            } else if (key == KEY(Page_Down)) {
                Vt_buffered_output(self, "\e[6~", 4);
                return true;
            }
        }
    }

    return false;
}

static const char* application_cursor_key_response(const uint32_t key)
{
    switch (key) {
        case KEY(Up):
            return "\eOA";
        case KEY(Down):
            return "\eOB";
        case KEY(Right):
            return "\eOC";
        case KEY(Left):
            return "\eOD";
        case KEY(End):
            return "\eOF";
        case KEY(Home):
            return "\eOH";
        case 127:
            return "\e[3~";
        default:
            return NULL;
    }
}

static const char* Vt_get_normal_cursor_key_response(Vt* self, const uint32_t key)
{
    if (self->modes.application_keypad_cursor) {
        return application_cursor_key_response(key);
    }

    switch (key) {
        case KEY(Up):
            return "\e[A";
        case KEY(Down):
            return "\e[B";
        case KEY(Right):
            return "\e[C";
        case KEY(Left):
            return "\e[D";
        case KEY(End):
            return "\e[F";
        case KEY(Home):
            return "\e[H";
        case 127:
            return "\e[3~";
        default:
            return NULL;
    }
}

/**
 * Get response format string in normal keypad mode */
static const char* mod_cursor_key_response(const uint32_t key)
{
    switch (key) {
        case KEY(Up):
            return "\e[1;%dA";
        case KEY(Down):
            return "\e[1;%dB";
        case KEY(Right):
            return "\e[1;%dC";
        case KEY(Left):
            return "\e[1;%dD";
        case KEY(End):
            return "\e[1;%dF";
        case KEY(Home):
            return "\e[1;%dH";
        case 127:
            return "\e[3;%d~";
        default:
            return NULL;
    }
}

/**
 * Respond to key event if it is a keypad key
 * @return keypress was consumed */
static bool Vt_maybe_handle_keypad_key(Vt* self, uint32_t key, uint32_t mods)
{
    const char* resp = NULL;
    if (mods) {
        resp = mod_cursor_key_response(key);
        if (resp) {
            Vt_buffered_output_formated(self, resp, mods + 1);
            return true;
        }
    } else {
        resp = Vt_get_normal_cursor_key_response(self, key);
        if (resp) {
            Vt_buffered_output(self, resp, strlen(resp));
            return true;
        }
    }

    return false;
}

/**
 * Respond to key event */
void Vt_handle_key(void* _self, uint32_t key, uint32_t rawkey, uint32_t mods)
{
    Vt* self = _self;

    key = numpad_key_convert(key);

    if (!Vt_maybe_handle_unicode_input_key(self, key, rawkey, mods) &&
        !Vt_maybe_handle_keypad_key(self, key, mods) &&
        !Vt_maybe_handle_function_key(self, key, mods)) {

        if (FLAG_IS_SET(mods, MODIFIER_ALT) && !self->modes.no_alt_sends_esc) {
            Vector_push_char(&self->output, '\e');
        }
        if (unlikely(FLAG_IS_SET(mods, MODIFIER_CONTROL))) {
            if (unlikely(key == ' ')) {
                key = 0;
            } else if (isalpha(key)) {
                key = tolower(key) - 'a';
            }
        }
        if (unlikely(key == '\b') && ((mods & MODIFIER_ALT) == mods || !mods) &&
            settings.bsp_sends_del) {
            key = 127;
        }
        char             tmp[32];
        static mbstate_t mbstate;
        size_t           mb_len = c32rtomb(tmp, key, &mbstate);
        if (mb_len) {
            Vt_buffered_output(self, tmp, mb_len);
        }
    }

    if (settings.scroll_on_key) {
        Vt_visual_scroll_reset(self);
    }
}
