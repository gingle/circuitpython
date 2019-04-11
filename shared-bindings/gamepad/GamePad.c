/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Radomir Dopieralski for Adafruit Industries
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
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/gc.h"
#include "py/mpstate.h"
#include "shared-module/gamepad/__init__.h"
#include "shared-module/gamepad/GamePad.h"
#include "shared-bindings/digitalio/DigitalInOut.h"
#include "shared-bindings/util.h"
#include "supervisor/shared/translate.h"
#include "GamePad.h"

STATIC digitalio_digitalinout_obj_t *validate_pin(mp_obj_t obj) {
    if (!MP_OBJ_IS_TYPE(obj, &digitalio_digitalinout_type)) {
        mp_raise_TypeError(translate("argument num/types mismatch"));
    }
    digitalio_digitalinout_obj_t *pin = MP_OBJ_TO_PTR(obj);
    raise_error_if_deinited(
        common_hal_digitalio_digitalinout_deinited(pin));
    return pin;
}

//| .. currentmodule:: gamepad
//|
//| :class:`GamePad` -- Scan buttons for presses
//| ============================================
//|
//| Usage::
//|
//|     import board
//|     import digitalio
//|     import gamepad
//|     import time
//|
//|     B_UP = 1 << 0
//|     B_DOWN = 1 << 1
//|
//|
//|     pad = gamepad.GamePad(
//|         digitalio.DigitalInOut(board.D10),
//|         digitalio.DigitalInOut(board.D11),
//|     )
//|
//|     y = 0
//|     while True:
//|         buttons = pad.get_pressed()
//|         if buttons & B_UP:
//|             y -= 1
//|             print(y)
//|         elif buttons & B_DOWN:
//|             y += 1
//|             print(y)
//|         time.sleep(0.1)
//|         while buttons:
//|             # Wait for all buttons to be released.
//|             buttons = pad.get_pressed()
//|             time.sleep(0.1)
//|

//| .. class:: GamePad([b1[, b2[, b3[, b4[, b5[, b6[, b7[, b8]]]]]]]])
//|
//|     Initializes button scanning routines.
//|
//|     The ``b1``-``b8`` parameters are ``DigitalInOut`` objects, which
//|     immediately get switched to input with a pull-up, and then scanned
//|     regularly for button presses. The order is the same as the order of
//|     bits returned by the ``get_pressed`` function. You can re-initialize
//|     it with different keys, then the new object will replace the previous
//|     one.
//|
//|     The basic feature required here is the ability to poll the keys at
//|     regular intervals (so that de-bouncing is consistent) and fast enough
//|     (so that we don't miss short button presses) while at the same time
//|     letting the user code run normally, call blocking functions and wait
//|     on delays.
//|
//|     They button presses are accumulated, until the ``get_pressed`` method
//|     is called, at which point the button state is cleared, and the new
//|     button presses start to be recorded.
//|
STATIC mp_obj_t gamepad_make_new(const mp_obj_type_t *type, size_t n_args,
        const mp_obj_t *args, mp_map_t *kw_args) {
    if (n_args > 8 || n_args == 0) {
        mp_raise_TypeError(translate("argument num/types mismatch"));
    }
    for (size_t i = 0; i < n_args; ++i) {
        validate_pin(args[i]);
    }
    gamepad_obj_t* gamepad_singleton = MP_STATE_VM(gamepad_singleton);
    if (!gamepad_singleton) {
        gamepad_singleton = m_new_obj(gamepad_obj_t);
        gamepad_singleton->base.type = &gamepadshift_type;
        MP_STATE_VM(gamepad_singleton) = gc_make_long_lived(gamepad_singleton);
    }
    for (size_t i = 0; i < 8; ++i) {
        gamepad_singleton->pins[i] = NULL;
    }
    gamepad_singleton->pulls = 0;
    for (size_t i = 0; i < n_args; ++i) {
        digitalio_digitalinout_obj_t *pin = MP_OBJ_TO_PTR(args[i]);
        if (common_hal_digitalio_digitalinout_get_direction(pin) !=
            DIRECTION_INPUT) {
            common_hal_digitalio_digitalinout_switch_to_input(pin, PULL_UP);
        }
        digitalio_pull_t pull = common_hal_digitalio_digitalinout_get_pull(pin);
        if (pull == PULL_NONE) {
            common_hal_digitalio_digitalinout_set_pull(pin, PULL_UP);
        }
        if (pull != PULL_DOWN) {
            gamepad_singleton->pulls |= 1 << i;
        }
        gamepad_singleton->pins[i] = pin;
    }
    return MP_OBJ_FROM_PTR(MP_STATE_VM(gamepad_singleton));
}


//| .. class:: GamePadShift(data, clock, latch)
//|
//|     Initializes button scanning routines.
//|
//|     The ``data``, ``clock`` and ``latch`` parameters are ``DigitalInOut``
//|     objects connected to the shift register controlling the buttons.
//|
//|     They button presses are accumulated, until the ``get_pressed`` method
//|     is called, at which point the button state is cleared, and the new
//|     button presses start to be recorded.
//|
STATIC mp_obj_t gamepadshift_make_new(const mp_obj_type_t *type, size_t n_args,
        const mp_obj_t *pos_args, mp_map_t *kw_args) {

    enum { ARG_data, ARG_clock, ARG_latch };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_clock, MP_ARG_REQUIRED | MP_ARG_OBJ},
        { MP_QSTR_latch, MP_ARG_REQUIRED | MP_ARG_OBJ},
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args),
                     allowed_args, args);

    digitalio_digitalinout_obj_t *data_pin = validate_pin(args[ARG_data].u_obj);
    digitalio_digitalinout_obj_t *clock_pin = validate_pin(args[ARG_clock].u_obj);
    digitalio_digitalinout_obj_t *latch_pin = validate_pin(args[ARG_latch].u_obj);

    gamepad_obj_t* gamepad_singleton = MP_STATE_VM(gamepad_singleton);
    if (!gamepad_singleton) {
        gamepad_singleton = m_new_obj(gamepad_obj_t);
        gamepad_singleton->base.type = &gamepadshift_type;
        MP_STATE_VM(gamepad_singleton) = gc_make_long_lived(gamepad_singleton);
    }
    gamepad_singleton->pins[0] = NULL;
    common_hal_digitalio_digitalinout_switch_to_input(data_pin, PULL_NONE);
    gamepad_singleton->pins[1] = data_pin;
    common_hal_digitalio_digitalinout_switch_to_output(clock_pin, 0,
                                                       DRIVE_MODE_PUSH_PULL);
    gamepad_singleton->pins[2] = clock_pin;
    common_hal_digitalio_digitalinout_switch_to_output(latch_pin, 1,
                                                       DRIVE_MODE_PUSH_PULL);
    gamepad_singleton->pins[3] = latch_pin;
    return MP_OBJ_FROM_PTR(MP_STATE_VM(gamepad_singleton));
}


//|     .. method:: get_pressed()
//|
//|         Get the status of buttons pressed since the last call and clear it.
//|
//|         Returns an 8-bit number, with bits that correspond to buttons,
//|         which have been pressed (or held down) since the last call to this
//|         function set to 1, and the remaining bits set to 0. Then it clears
//|         the button state, so that new button presses (or buttons that are
//|         held down) can be recorded for the next call.
//|
STATIC mp_obj_t gamepad_get_pressed(mp_obj_t self_in) {
    gamepad_obj_t* gamepad_singleton = MP_STATE_VM(gamepad_singleton);
    mp_obj_t pressed = MP_OBJ_NEW_SMALL_INT(gamepad_singleton->pressed);
    gamepad_singleton->pressed = 0;
    return pressed;
}
MP_DEFINE_CONST_FUN_OBJ_1(gamepad_get_pressed_obj, gamepad_get_pressed);


//|     .. method:: deinit()
//|
//|         Disable button scanning.
//|
STATIC mp_obj_t gamepad_deinit(mp_obj_t self_in) {
    gamepad_reset();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gamepad_deinit_obj, gamepad_deinit);


STATIC const mp_rom_map_elem_t gamepad_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_pressed),  MP_ROM_PTR(&gamepad_get_pressed_obj)},
    { MP_OBJ_NEW_QSTR(MP_QSTR_deinit),  MP_ROM_PTR(&gamepad_deinit_obj)},
};
STATIC MP_DEFINE_CONST_DICT(gamepad_locals_dict, gamepad_locals_dict_table);
const mp_obj_type_t gamepad_type = {
    { &mp_type_type },
    .name = MP_QSTR_GamePad,
    .make_new = gamepad_make_new,
    .locals_dict = (mp_obj_dict_t*)&gamepad_locals_dict,
};

STATIC const mp_rom_map_elem_t gamepadshift_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR_get_pressed),  MP_ROM_PTR(&gamepad_get_pressed_obj)},
    { MP_OBJ_NEW_QSTR(MP_QSTR_deinit),  MP_ROM_PTR(&gamepad_deinit_obj)},
};
STATIC MP_DEFINE_CONST_DICT(gamepadshift_locals_dict, gamepadshift_locals_dict_table);
const mp_obj_type_t gamepadshift_type = {
    { &mp_type_type },
    .name = MP_QSTR_GamePadShift,
    .make_new = gamepadshift_make_new,
    .locals_dict = (mp_obj_dict_t*)&gamepadshift_locals_dict,
};
