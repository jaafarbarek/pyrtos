/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "py/nlr.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#ifdef MICROPY_HAL_H
#include MICROPY_HAL_H
#endif
#if defined(USE_DEVICE_MODE)
#include "irq.h"
#include "usb.h"
#endif
#include "readline.h"
#include "pyexec.h"
#include "genhdr/mpversion.h"

pyexec_mode_kind_t pyexec_mode_kind = PYEXEC_MODE_FRIENDLY_REPL;
STATIC bool repl_display_debugging_info = 0;

#define EXEC_FLAG_PRINT_EOF (1)
#define EXEC_FLAG_ALLOW_DEBUGGING (2)
#define EXEC_FLAG_IS_REPL (4)

// parses, compiles and executes the code in the lexer
// frees the lexer before returning
// EXEC_FLAG_PRINT_EOF prints 2 EOF chars: 1 after normal output, 1 after exception output
// EXEC_FLAG_ALLOW_DEBUGGING allows debugging info to be printed after executing the code
// EXEC_FLAG_IS_REPL is used for REPL inputs (flag passed on to mp_compile)
STATIC int parse_compile_execute(mp_lexer_t *lex, mp_parse_input_kind_t input_kind, int exec_flags) {
    int ret = 0;
    uint32_t start = 0;

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        // parse and compile the script
        qstr source_name = lex->source_name;
        mp_parse_tree_t parse_tree = mp_parse(lex, input_kind);
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, MP_EMIT_OPT_NONE, exec_flags & EXEC_FLAG_IS_REPL);

        // execute code
        mp_hal_set_interrupt_char(CHAR_CTRL_C); // allow ctrl-C to interrupt us
        start = HAL_GetTick();
        mp_call_function_0(module_fun);
        mp_hal_set_interrupt_char(-1); // disable interrupt
        nlr_pop();
        ret = 1;
        if (exec_flags & EXEC_FLAG_PRINT_EOF) {
            mp_hal_stdout_tx_strn("\x04", 1);
        }
    } else {
        // uncaught exception
        // FIXME it could be that an interrupt happens just before we disable it here
        mp_hal_set_interrupt_char(-1); // disable interrupt
        // print EOF after normal output
        if (exec_flags & EXEC_FLAG_PRINT_EOF) {
            mp_hal_stdout_tx_strn("\x04", 1);
        }
        // check for SystemExit
        if (mp_obj_is_subclass_fast(mp_obj_get_type((mp_obj_t)nlr.ret_val), &mp_type_SystemExit)) {
            // at the moment, the value of SystemExit is unused
            ret = PYEXEC_FORCED_EXIT;
        } else {
            mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
            ret = 0;
        }
    }

    // display debugging info if wanted
    if ((exec_flags & EXEC_FLAG_ALLOW_DEBUGGING) && repl_display_debugging_info) {
        mp_uint_t ticks = HAL_GetTick() - start; // TODO implement a function that does this properly
        printf("took " UINT_FMT " ms\n", ticks);
        gc_collect();
        // qstr info
        {
            mp_uint_t n_pool, n_qstr, n_str_data_bytes, n_total_bytes;
            qstr_pool_info(&n_pool, &n_qstr, &n_str_data_bytes, &n_total_bytes);
            printf("qstr:\n  n_pool=" UINT_FMT "\n  n_qstr=" UINT_FMT "\n  n_str_data_bytes=" UINT_FMT "\n  n_total_bytes=" UINT_FMT "\n", n_pool, n_qstr, n_str_data_bytes, n_total_bytes);
        }

        // GC info
        gc_dump_info();
    }

    if (exec_flags & EXEC_FLAG_PRINT_EOF) {
        mp_hal_stdout_tx_strn("\x04", 1);
    }

    return ret;
}

#if MICROPY_REPL_EVENT_DRIVEN

typedef struct _repl_t {
    // XXX line holds a root pointer!
    vstr_t line;
    bool cont_line;
} repl_t;

repl_t repl;

STATIC int pyexec_raw_repl_process_char(int c);
STATIC int pyexec_friendly_repl_process_char(int c);

void pyexec_event_repl_init(void) {
    vstr_init(&repl.line, 32);
    repl.cont_line = false;
    readline_init(&repl.line, ">>> ");
    if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
        pyexec_raw_repl_process_char(CHAR_CTRL_A);
    } else {
        pyexec_friendly_repl_process_char(CHAR_CTRL_B);
    }
}

STATIC int pyexec_raw_repl_process_char(int c) {
    if (c == CHAR_CTRL_A) {
        // reset raw REPL
        mp_hal_stdout_tx_str("raw REPL; CTRL-B to exit\r\n");
        goto reset;
    } else if (c == CHAR_CTRL_B) {
        // change to friendly REPL
        pyexec_mode_kind = PYEXEC_MODE_FRIENDLY_REPL;
        repl.cont_line = false;
        pyexec_friendly_repl_process_char(CHAR_CTRL_B);
        return 0;
    } else if (c == CHAR_CTRL_C) {
        // clear line
        vstr_reset(&repl.line);
        return 0;
    } else if (c == CHAR_CTRL_D) {
        // input finished
    } else {
        // let through any other raw 8-bit value
        vstr_add_byte(&repl.line, c);
        return 0;
    }

    // indicate reception of command
    mp_hal_stdout_tx_str("OK");

    if (repl.line.len == 0) {
        // exit for a soft reset
        mp_hal_stdout_tx_str("\r\n");
        vstr_clear(&repl.line);
        return PYEXEC_FORCED_EXIT;
    }

    mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, repl.line.buf, repl.line.len, 0);
    if (lex == NULL) {
        mp_hal_stdout_tx_str("\x04MemoryError\r\n\x04");
    } else {
        int ret = parse_compile_execute(lex, MP_PARSE_FILE_INPUT, EXEC_FLAG_PRINT_EOF);
        if (ret & PYEXEC_FORCED_EXIT) {
            return ret;
        }
    }

reset:
    vstr_reset(&repl.line);
    mp_hal_stdout_tx_str(">");

    return 0;
}

STATIC int pyexec_friendly_repl_process_char(int c) {
    int ret = readline_process_char(c);

    if (!repl.cont_line) {

        if (ret == CHAR_CTRL_A) {
            // change to raw REPL
            pyexec_mode_kind = PYEXEC_MODE_RAW_REPL;
            mp_hal_stdout_tx_str("\r\n");
            pyexec_raw_repl_process_char(CHAR_CTRL_A);
            return 0;
        } else if (ret == CHAR_CTRL_B) {
            // reset friendly REPL
            mp_hal_stdout_tx_str("\r\n");
            mp_hal_stdout_tx_str("MicroPython " MICROPY_GIT_TAG " on " MICROPY_BUILD_DATE "; " MICROPY_HW_BOARD_NAME " with " MICROPY_HW_MCU_NAME "\r\n");
            mp_hal_stdout_tx_str("Type \"help()\" for more information.\r\n");
            goto input_restart;
        } else if (ret == CHAR_CTRL_C) {
            // break
            mp_hal_stdout_tx_str("\r\n");
            goto input_restart;
        } else if (ret == CHAR_CTRL_D) {
            // exit for a soft reset
            mp_hal_stdout_tx_str("\r\n");
            vstr_clear(&repl.line);
            return PYEXEC_FORCED_EXIT;
        }

        if (ret < 0) {
            return 0;
        }

        if (!mp_repl_continue_with_input(vstr_null_terminated_str(&repl.line))) {
            goto exec;
        }

        vstr_add_byte(&repl.line, '\n');
        repl.cont_line = true;
        readline_note_newline("... ");
        return 0;

    } else {

        if (ret == CHAR_CTRL_C) {
           // cancel everything
           mp_hal_stdout_tx_str("\r\n");
           repl.cont_line = false;
           goto input_restart;
        } else if (ret == CHAR_CTRL_D) {
            // stop entering compound statement
            goto exec;
        }

        if (ret < 0) {
            return 0;
        }

        if (mp_repl_continue_with_input(vstr_null_terminated_str(&repl.line))) {
            vstr_add_byte(&repl.line, '\n');
            readline_note_newline("... ");
            return 0;
        }

exec: ;
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, vstr_str(&repl.line), vstr_len(&repl.line), 0);
        if (lex == NULL) {
            printf("MemoryError\n");
        } else {
            int ret = parse_compile_execute(lex, MP_PARSE_SINGLE_INPUT, EXEC_FLAG_ALLOW_DEBUGGING | EXEC_FLAG_IS_REPL);
            if (ret & PYEXEC_FORCED_EXIT) {
                return ret;
            }
        }

input_restart:
        vstr_reset(&repl.line);
        repl.cont_line = false;
        readline_init(&repl.line, ">>> ");
        return 0;
    }
}

int pyexec_event_repl_process_char(int c) {
    if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
        return pyexec_raw_repl_process_char(c);
    } else {
        return pyexec_friendly_repl_process_char(c);
    }
}

#else // MICROPY_REPL_EVENT_DRIVEN

int pyexec_raw_repl(void) {
    vstr_t line;
    vstr_init(&line, 32);

raw_repl_reset:
    mp_hal_stdout_tx_str("raw REPL; CTRL-B to exit\r\n");

    for (;;) {
        vstr_reset(&line);
        mp_hal_stdout_tx_str(">");
        for (;;) {
            int c = mp_hal_stdin_rx_chr();
            if (c == CHAR_CTRL_A) {
                // reset raw REPL
                goto raw_repl_reset;
            } else if (c == CHAR_CTRL_B) {
                // change to friendly REPL
                mp_hal_stdout_tx_str("\r\n");
                vstr_clear(&line);
                pyexec_mode_kind = PYEXEC_MODE_FRIENDLY_REPL;
                return 0;
            } else if (c == CHAR_CTRL_C) {
                // clear line
                vstr_reset(&line);
            } else if (c == CHAR_CTRL_D) {
                // input finished
                break;
            } else {
                // let through any other raw 8-bit value
                vstr_add_byte(&line, c);
            }
        }

        // indicate reception of command
        mp_hal_stdout_tx_str("OK");

        if (line.len == 0) {
            // exit for a soft reset
            mp_hal_stdout_tx_str("\r\n");
            vstr_clear(&line);
            return PYEXEC_FORCED_EXIT;
        }

        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, line.buf, line.len, 0);
        if (lex == NULL) {
            printf("\x04MemoryError\n\x04");
        } else {
            int ret = parse_compile_execute(lex, MP_PARSE_FILE_INPUT, EXEC_FLAG_PRINT_EOF);
            if (ret & PYEXEC_FORCED_EXIT) {
                return ret;
            }
        }
    }
}

int pyexec_friendly_repl(void) {
    vstr_t line;
    vstr_init(&line, 32);

#if defined(USE_HOST_MODE) && MICROPY_HW_HAS_LCD
    // in host mode, we enable the LCD for the repl
    mp_obj_t lcd_o = mp_call_function_0(mp_load_name(qstr_from_str("LCD")));
    mp_call_function_1(mp_load_attr(lcd_o, qstr_from_str("light")), mp_const_true);
#endif

friendly_repl_reset:
    mp_hal_stdout_tx_str("MicroPython " MICROPY_GIT_TAG " on " MICROPY_BUILD_DATE "; " MICROPY_HW_BOARD_NAME " with " MICROPY_HW_MCU_NAME "\r\n");
    mp_hal_stdout_tx_str("Type \"help()\" for more information.\r\n");

    // to test ctrl-C
    /*
    {
        uint32_t x[4] = {0x424242, 0xdeaddead, 0x242424, 0xdeadbeef};
        for (;;) {
            nlr_buf_t nlr;
            printf("pyexec_repl: %p\n", x);
            mp_hal_set_interrupt_char(CHAR_CTRL_C);
            if (nlr_push(&nlr) == 0) {
                for (;;) {
                }
            } else {
                printf("break\n");
            }
        }
    }
    */

    for (;;) {
    input_restart:

        #if defined(USE_DEVICE_MODE)
        if (usb_vcp_is_enabled()) {
            // If the user gets to here and interrupts are disabled then
            // they'll never see the prompt, traceback etc. The USB REPL needs
            // interrupts to be enabled or no transfers occur. So we try to
            // do the user a favor and reenable interrupts.
            if (query_irq() == IRQ_STATE_DISABLED) {
                enable_irq(IRQ_STATE_ENABLED);
                mp_hal_stdout_tx_str("PYB: enabling IRQs\r\n");
            }
        }
        #endif

        vstr_reset(&line);
        int ret = readline(&line, ">>> ");
        mp_parse_input_kind_t parse_input_kind = MP_PARSE_SINGLE_INPUT;

        if (ret == CHAR_CTRL_A) {
            // change to raw REPL
            mp_hal_stdout_tx_str("\r\n");
            vstr_clear(&line);
            pyexec_mode_kind = PYEXEC_MODE_RAW_REPL;
            return 0;
        } else if (ret == CHAR_CTRL_B) {
            // reset friendly REPL
            mp_hal_stdout_tx_str("\r\n");
            goto friendly_repl_reset;
        } else if (ret == CHAR_CTRL_C) {
            // break
            mp_hal_stdout_tx_str("\r\n");
            continue;
        } else if (ret == CHAR_CTRL_D) {
            // exit for a soft reset
            mp_hal_stdout_tx_str("\r\n");
            vstr_clear(&line);
            return PYEXEC_FORCED_EXIT;
        } else if (ret == CHAR_CTRL_E) {
            // paste mode
            mp_hal_stdout_tx_str("\r\npaste mode; CTRL-C to cancel, CTRL-D to finish\r\n=== ");
            vstr_reset(&line);
            for (;;) {
                char c = mp_hal_stdin_rx_chr();
                if (c == CHAR_CTRL_C) {
                    // cancel everything
                    mp_hal_stdout_tx_str("\r\n");
                    goto input_restart;
                } else if (c == CHAR_CTRL_D) {
                    // end of input
                    mp_hal_stdout_tx_str("\r\n");
                    break;
                } else {
                    // add char to buffer and echo
                    vstr_add_byte(&line, c);
                    if (c == '\r') {
                        mp_hal_stdout_tx_str("\r\n=== ");
                    } else {
                        mp_hal_stdout_tx_strn(&c, 1);
                    }
                }
            }
            parse_input_kind = MP_PARSE_FILE_INPUT;
        } else if (vstr_len(&line) == 0) {
            continue;
        } else {
            // got a line with non-zero length, see if it needs continuing
            while (mp_repl_continue_with_input(vstr_null_terminated_str(&line))) {
                vstr_add_byte(&line, '\n');
                ret = readline(&line, "... ");
                if (ret == CHAR_CTRL_C) {
                    // cancel everything
                    mp_hal_stdout_tx_str("\r\n");
                    goto input_restart;
                } else if (ret == CHAR_CTRL_D) {
                    // stop entering compound statement
                    break;
                }
            }
        }

        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, vstr_str(&line), vstr_len(&line), 0);
        if (lex == NULL) {
            printf("MemoryError\n");
        } else {
            ret = parse_compile_execute(lex, parse_input_kind, EXEC_FLAG_ALLOW_DEBUGGING | EXEC_FLAG_IS_REPL);
            if (ret & PYEXEC_FORCED_EXIT) {
                return ret;
            }
        }
    }
}

#endif // MICROPY_REPL_EVENT_DRIVEN

int pyexec_file(const char *filename) {
    mp_lexer_t *lex = mp_lexer_new_from_file(filename);

    if (lex == NULL) {
        printf("could not open file '%s' for reading\n", filename);
        return false;
    }

    return parse_compile_execute(lex, MP_PARSE_FILE_INPUT, 0);
}

mp_obj_t pyb_set_repl_info(mp_obj_t o_value) {
    repl_display_debugging_info = mp_obj_get_int(o_value);
    return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_1(pyb_set_repl_info_obj, pyb_set_repl_info);
