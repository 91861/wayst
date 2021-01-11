#pragma once

#include "util.h"
#include "vt.h"

static void Vt_shell_integration_begin_prompt(Vt* self)
{
    self->shell_integration_state = VT_SHELL_INTEG_STATE_PROMPT;
}

static void Vt_shell_integration_begin_command(Vt* self)
{
    RcPtr_VtCommand new_command        = RcPtr_new_VtCommand();
    *RcPtr_get_VtCommand(&new_command) = (VtCommand){
        .command              = NULL,
        .command_start_row    = self->cursor.row,
        .command_start_column = self->cursor.col,
        .state                = VT_COMMAND_STATE_TYPING,
    };

    for (VtCommand* c;
         (c = RcPtr_get_VtCommand(Vector_last_RcPtr_VtCommand(&self->shell_commands))) &&
         c->state != VT_COMMAND_STATE_COMPLETED;) {
        Vector_pop_RcPtr_VtCommand(&self->shell_commands);
    }

    Vector_push_RcPtr_VtCommand(&self->shell_commands, new_command);

    self->shell_integration_state = VT_SHELL_INTEG_STATE_COMMAND;
}

static void Vt_shell_integration_begin_execution(Vt*  self,
                                                 bool no_name_search,
                                                 bool is_vte_protocol)
{
    RcPtr_VtCommand* cmd_ptr = Vector_last_RcPtr_VtCommand(&self->shell_commands);
    VtCommand*       cmd     = NULL;

    if (!cmd_ptr || !(cmd = RcPtr_get_VtCommand(cmd_ptr))) {
        self->shell_integration_state = VT_SHELL_INTEG_STATE_NONE;
        return;
    }

    if (cmd->command_start_row > self->cursor.row ||
        (cmd->command_start_row == self->cursor.row &&
         cmd->command_start_column >= self->cursor.col)) {
        self->shell_integration_state = VT_SHELL_INTEG_STATE_NONE;
        return;
    }

    cmd->state           = VT_COMMAND_STATE_RUNNING;
    cmd->is_vte_protocol = is_vte_protocol;

    for (size_t i = cmd->command_start_row; i < self->cursor.row; ++i) {
        Vt_line_at(self, i)->mark_command_invoke = true;
    }

    Vt_cursor_line(self)->mark_command_output_start = true;
    RcPtr_new_shared_in_place_of_VtCommand(&Vt_cursor_line(self)->linked_command, cmd_ptr);

    cmd->output_rows.first    = self->cursor.row;
    cmd->execution_time.start = TimePoint_now();

    Vector_char command_string_builder;

    if (!no_name_search) {
        if (cmd->command_start_row == self->cursor.row - 1) {
            VtLine* ln              = Vt_line_at(self, cmd->command_start_row);
            ln->mark_command_invoke = true;
            command_string_builder =
              VtLine_to_string(ln, cmd->command_start_column, self->cursor.col, NULL);

            for (char* r; command_string_builder.size &&
                          (r = Vector_last_char(&command_string_builder)) && *r == ' ';) {
                Vector_pop_char(&command_string_builder);
            }

        } else {
            VtLine* ln              = Vt_line_at(self, cmd->command_start_row);
            ln->mark_command_invoke = true;
            command_string_builder =
              VtLine_to_string(ln, cmd->command_start_column, Vt_col(self) - 1, NULL);

            for (char* r; command_string_builder.size &&
                          (r = Vector_last_char(&command_string_builder)) && *r == ' ';) {
                Vector_pop_char(&command_string_builder);
            }

            Vector_push_char(&command_string_builder, '\n');

            for (size_t row = cmd->command_start_row + 1; row < self->cursor.row; ++row) {
                ln                      = Vt_line_at(self, row);
                ln->mark_command_invoke = true;
                Vector_char tmp         = VtLine_to_string(ln, 0, Vt_col(self), NULL);

                for (char* r; command_string_builder.size &&
                              (r = Vector_last_char(&command_string_builder)) && *r == ' ';) {
                    Vector_pop_char(&command_string_builder);
                }

                if (row != self->cursor.row - 1) {
                    Vector_push_char(&command_string_builder, '\n');
                }

                if (tmp.size) {
                    Vector_pushv_char(&command_string_builder, tmp.buf, tmp.size - 1);
                }
                Vector_destroy_char(&tmp);
            }
        }

        Vector_push_char(&command_string_builder, '\0');
        cmd->command = strdup(command_string_builder.buf);
        Vector_destroy_char(&command_string_builder);
    } else {
        cmd->command = NULL;
    }

    self->shell_integration_state = VT_SHELL_INTEG_STATE_OUTPUT;
    CALL(self->callbacks.on_command_state_changed, self->callbacks.user_data);
}

static void Vt_shell_integration_active_command_name_changed(Vt* self, const char* command)
{
    RcPtr_VtCommand* cmd_ptr = Vector_last_RcPtr_VtCommand(&self->shell_commands);
    VtCommand*       cmd     = NULL;

    if (!cmd_ptr || !(cmd = RcPtr_get_VtCommand(cmd_ptr))) {
        return;
    }

    free(cmd->command);
    cmd->command = strdup(command);
}

static void Vt_shell_integration_end_execution(Vt* self, const char* opt_exit_status_string)
{
    RcPtr_VtCommand* cmd_ptr = Vector_last_RcPtr_VtCommand(&self->shell_commands);
    VtCommand*       cmd     = NULL;

    if (!cmd_ptr || !(cmd = RcPtr_get_VtCommand(cmd_ptr))) {
        self->shell_integration_state = VT_SHELL_INTEG_STATE_NONE;
        return;
    }

    cmd->state              = VT_COMMAND_STATE_COMPLETED;
    cmd->exit_status        = opt_exit_status_string ? atoi(opt_exit_status_string) : 0;
    cmd->execution_time.end = TimePoint_now();
    cmd->output_rows.second = self->cursor.row;

    VtLine* ln = Vt_line_at(self, self->cursor.row - 1);

    if (ln) {
        ln->mark_command_output_end = true;

        bool minimized =
          CALL(self->callbacks.on_minimized_state_requested, self->callbacks.user_data);

        LOG("Vt::command_finished{ command: \'%s\' [%u:%zu], status: %d, output: "
            "%zu..%zu }\n",
            cmd->command,
            cmd->command_start_column,
            cmd->command_start_row,
            cmd->exit_status,
            cmd->output_rows.first,
            cmd->output_rows.second);

        CALL(self->callbacks.on_urgency_set, self->callbacks.user_data);
        if (minimized && cmd->command) {
            char* tm_str = TimeSpan_duration_string_approx(&cmd->execution_time);
            char* notification_title =
              cmd->exit_status
                ? asprintf("\'%s\' failed(%d), took %s", cmd->command, cmd->exit_status, tm_str)
                : asprintf("\'%s\' finished in %s", cmd->command, tm_str);

            Vector_char output = Vt_command_to_string(self, cmd, 1);

            if (output.size > 32) {
                for (uint32_t i = 0; i < (output.size - 32); ++i) {
                    Vector_pop_char(&output);
                }
                Vector_pushv_char(&output, "…", strlen("…") + 1);
            }

            CALL(self->callbacks.on_desktop_notification_sent,
                 self->callbacks.user_data,
                 notification_title,
                 output.buf);

            Vector_destroy_char(&output);
            free(notification_title);
            free(tm_str);
        }
    }

    self->shell_integration_state = VT_SHELL_INTEG_STATE_NONE;
    CALL(self->callbacks.on_command_state_changed, self->callbacks.user_data);
}
