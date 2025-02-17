/* Copyright 2023 Dimitris Mantzouranis <d3xter93@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "sled1734x.h"
#include "i2c_master.h"
#include "wait.h"

#ifndef SLED1734X_TIMEOUT
#    define SLED1734X_TIMEOUT 100
#endif

#ifndef SLED1734X_PERSISTENCE
#    define SLED1734X_PERSISTENCE 0
#endif

// Transfer buffer for TWITransmitData()
uint8_t i2c_transfer_buffer[65];

// These buffers match the SLED1734X PWM registers 0x20-0x9F.
// Storing them like this is optimal for I2C transfers to the registers.
// We could optimize this and take out the unused registers from these
// buffers and the transfers in sled1734x_write_pwm_buffer() but it's
// probably not worth the extra complexity.
uint8_t g_pwm_buffer[SLED1734X_DRIVER_COUNT][256];
bool    g_pwm_buffer_update_required[SLED1734X_DRIVER_COUNT] = {false};

uint8_t g_led_control_registers[SLED1734X_DRIVER_COUNT][32]             = {{0}};
bool    g_led_control_registers_update_required[SLED1734X_DRIVER_COUNT] = {false};

// This is the bit pattern in the LED control registers
// (for matrix type 3, using split frames)
//
//  CHA - reg  - b15  b14  b13  b12  b11  b10  b09  b08  b07  b06  b05  b04  b03  b02  b01  b00
// --------------------------------------------------------------------------------------------
//  CA1 - 0x00 | R13, R12, R11, R10, R09, R08, R07, R06, R05, R04, R03, R02, R01, R00,  - ,  -
//  CA2 - 0x02 | G13, G12, G11, G10, G09, G08, G07, G06, G05, G04, G03, G02, G01, G00,  - ,  -
//  CA3 - 0x04 | B13, B12, B11, B10, B09, B08, B07, B06, B05, B04, B03, B02, B01, B00,  - ,  -
//  CA4 - 0x06 | R27, R26, R25, R24, R23, R22, R21, R20, R19, R18, R17,  - ,  - , R16, R15, R14
//  CA5 - 0x08 | G27, G26, G25, G24, G23, G22, G21, G20, G19, G18, G17,  - ,  - , G16, G15, G14
//  CA6 - 0x0A | B27, B26, B25, B24, B23, B22, B21, B20, B19, B18, B17,  - ,  - , B16, B15, B14
//  CA7 - 0x0C | R41, R40, R39, R38, R37, R36, R35, R34,  - ,  - , R33, R32, R31, R30, R29, R28
//  CA8 - 0x0E | G41, G40, G39, G38, G37, G36, G35, G34,  - ,  - , G33, G32, G31, G30, G29, G28
// --------------------------------------------------------------------------------------
//  CA9 - 0x00 | B41, B40, B39, B38, B37, B36, B35, B34,  - ,  - , B33, B32, B31, B30, B29, B28
//  CB1 - 0x02 | R55, R54, R53, R52, R51,  - ,  - , R50, R49, R48, R47, R46, R45, R44, R43, R42
//  CB2 - 0x04 | G55, G54, G53, G52, G51,  - ,  - , G50, G49, G48, G47, G46, G45, G44, G43, G42
//  CB3 - 0x06 | B55, B54, B53, B52, B51,  - ,  - , B50, B49, B48, B47, B46, B45, B44, B43, B42
//  CB4 - 0x08 | R69, R68,  - ,  - , R67, R66, R65, R64, R64, R62, R61, R60, R59, R58, R57, R56
//  CB5 - 0x0A | G69, G68,  - ,  - , G67, G66, G65, G64, G64, G62, G61, G60, G59, G58, G57, G56
//  CB6 - 0x0C | B69, B68,  - ,  - , B67, B66, B65, B64, B64, B62, B61, B60, B59, B58, B57, B56
//  CB7 - 0x0E |  - ,  - ,  - ,  - ,  - ,  - ,  - ,  - ,  - ,  - ,  - ,  - ,  - ,  - ,  - ,  -
// --------------------------------------------------------------------------------------

void sled1734x_write_register(uint8_t addr, uint8_t reg, uint8_t data) {
    i2c_transfer_buffer[0] = reg;
    i2c_transfer_buffer[1] = data;

#if SLED1734X_PERSISTENCE > 0
    for (uint8_t i = 0; i < SLED1734X_PERSISTENCE; i++) {
        if (i2c_transmit(addr << 1, i2c_transfer_buffer, 2, SLED1734X_TIMEOUT) == 0) break;
    }
#else
    i2c_transmit(addr << 1, i2c_transfer_buffer, 2, SLED1734X_TIMEOUT);
#endif
}

void sled1734x_select_page(uint8_t addr, uint8_t page) {
    sled1734x_write_register(addr, SLED1734X_REG_COMMAND, page);
}

bool sled1734x_write_pwm_buffer(uint8_t addr, uint8_t *pwm_buffer) {
    // select the first frame
    sled1734x_select_page(addr, SLED1734X_COMMAND_FRAME_1);
    // transmit PWM registers in 2 transfers of 64 bytes
    // i2c_transfer_buffer[] is 65 bytes

    // iterate over the pwm_buffer contents at 64 byte intervals
    for (int i = 0; i < SLED1734X_FRAME_OFFSET; i += 64) {
        // set the first register, e.g. 0x20, 0x30, 0x40, etc.
        i2c_transfer_buffer[0] = SLED1734X_OFFSET + i;
        // copy the data from i to i+63
        // device will auto-increment register for data after the first byte
        // thus this sets registers 0x20-0x2F, 0x30-0x3F, etc. in one transfer
        for (int j = 0; j < 64; j++) {
            i2c_transfer_buffer[1 + j] = pwm_buffer[i + j];
        }

#if SLED1734X_PERSISTENCE > 0
        for (uint8_t i = 0; i < SLED1734X_PERSISTENCE; i++) {
            if (i2c_transmit(addr << 1, i2c_transfer_buffer, 65, SLED1734X_TIMEOUT) != 0) return false;
        }
#else
        if (i2c_transmit(addr << 1, i2c_transfer_buffer, 65, SLED1734X_TIMEOUT) != 0) return false;
#endif
    }
    // select the second frame
    sled1734x_select_page(addr, SLED1734X_COMMAND_FRAME_2);
    // transmit PWM registers in 2 transfers of 64 bytes
    // i2c_transfer_buffer[] is 65 bytes

    // iterate over the pwm_buffer contents at 16 byte intervals
    for (int i = 0; i < SLED1734X_FRAME_OFFSET; i += 64) {
        // set the first register, e.g. 0x20, 0x30, 0x40, etc.
        i2c_transfer_buffer[0] = SLED1734X_OFFSET + i;
        // copy the data from i to i+63
        // device will auto-increment register for data after the first byte
        // thus this sets registers 0x20-0x2f, 0x30-0x3f, etc. in one transfer
        for (int j = 0; j < 64; j++) {
            i2c_transfer_buffer[1 + j] = pwm_buffer[SLED1734X_FRAME_OFFSET + i + j];
        }

#if SLED1734X_PERSISTENCE > 0
        for (uint8_t i = 0; i < SLED1734X_PERSISTENCE; i++) {
            if (i2c_transmit(addr << 1, i2c_transfer_buffer, 65, SLED1734X_TIMEOUT) != 0) return false;
        }
#else
        if (i2c_transmit(addr << 1, i2c_transfer_buffer, 65, SLED1734X_TIMEOUT) != 0) return false;
#endif
    }
    return true;
}

void sled1734x_init_drivers(void) {
    i2c_init();
    sled1734x_init(SLED1734X_I2C_ADDRESS_1);
#if defined(SLED1734X_I2C_ADDRESS_2)
    sled1734x_init(SLED1734X_I2C_ADDRESS_2);
#    if defined(SLED1734X_I2C_ADDRESS_3)
    sled1734x_init(SLED1734X_I2C_ADDRESS_3);
#        if defined(SLED1734X_I2C_ADDRESS_4)
    sled1734x_init(SLED1734X_I2C_ADDRESS_4);
#        endif
#    endif
#endif

    for (int i = 0; i < SLED1734X_LED_COUNT; i++) {
        sled1734x_set_led_control_register(i, true, true, true);
    }

    sled1734x_update_led_control_registers(SLED1734X_I2C_ADDRESS_1, 0);
#if defined(SLED1734X_I2C_ADDRESS_2)
    sled1734x_update_led_control_registers(SLED1734X_I2C_ADDRESS_2, 1);
#    if defined(SLED1734X_I2C_ADDRESS_3)
    sled1734x_update_led_control_registers(SLED1734X_I2C_ADDRESS_3, 2);
#        if defined(SLED1734X_I2C_ADDRESS_4)
    sled1734x_update_led_control_registers(SLED1734X_I2C_ADDRESS_4, 3);
#        endif
#    endif
#endif
}

void sled1734x_init(uint8_t addr) {
    // Toggle the SDB pin HIGH to disable the hardware power down state
    // Not always connected to the MCU, hence optional here.
#ifdef SLED1734X_SDB_PIN
    setPinOutput(SLED1734X_SDB_PIN);
    writePinHigh(SLED1734X_SDB_PIN);
#endif
    // Hardware powerup requires 180us.
    wait_us(180);
    // In order to avoid the LEDs being driven with garbage data
    // in the LED driver's PWM registers, first enable software shutdown,
    // then set up the mode and other settings, clear the PWM registers,
    // then disable software shutdown.

    sled1734x_sw_shutdown(addr);
    // sync mode
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_CONFIGURATION, SLED1734X_SYNC_MODE);
    // matrix type
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_PICTUREDISPLAY, SLED1734X_MATRIX_TYPE);
    // blink frame
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_DISPLAYOPTION, SLED1734X_BLINK_FRAME);
    // audio sync off
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_AUDIOSYNC, SLED1734X_AUDIOSYNC_ENABLE);
    // breathe control
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_BREATHCONTROL1, SLED1734X_FADE_TIME);
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_BREATHCONTROL2, SLED1734X_BREATHE_ENABLE);
    // audio gain off
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_AUDIOGAIN_CONTROL, SLED1734X_AUDIOGAIN_MODE);
    // staggered delay off
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_STAGGERED_DELAY, SLED1734X_STAGGERED_DELAY_TIMING);
    // slew rate control enable
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_SLEW_RATE_CONTROL, SLED1734X_SLEW_RATE_CONTROL_ENABLE);
    // VAF fine tuning
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_VAF_1, SLED1734X_VAF_1_TUNE);
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_VAF_2, SLED1734X_VAF_2_TUNE);
    // current control
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_CURRENT_CONTROL, SLED1734X_CURRENT_CONTROL_ENABLE);

    // select page frame 1
    sled1734x_select_page(addr, SLED1734X_COMMAND_FRAME_1);

    // turn off all LEDs in the LED control register
    for (int i = 0x00; i <= 0x0F; i++) {
        sled1734x_write_register(addr, i, 0x00);
    }

    // turn off all LEDs in the blink control register (not really needed)
    for (int i = 0x10; i <= 0x1F; i++) {
        sled1734x_write_register(addr, i, 0x00);
    }

    // set PWM on all LEDs to 0
    for (int i = 0x20; i <= 0x9F; i++) {
        sled1734x_write_register(addr, i, 0x00);
    }

    // select page frame 2
    sled1734x_select_page(addr, SLED1734X_COMMAND_FRAME_2);

    // turn off all LEDs in the LED control register
    for (int i = 0x00; i <= 0x0F; i++) {
        sled1734x_write_register(addr, i, 0x00);
    }

    // turn off all LEDs in the blink control register (not really needed)
    for (int i = 0x10; i <= 0x1F; i++) {
        sled1734x_write_register(addr, i, 0x00);
    }

    // set PWM on all LEDs to 0
    for (int i = 0x20; i <= 0x9F; i++) {
        sled1734x_write_register(addr, i, 0x00);
    }

    sled1734x_sw_return_normal(addr);
}

void sled1734x_set_color(int index, uint8_t red, uint8_t green, uint8_t blue) {
    sled1734x_led led;
    if (index >= 0 && index < SLED1734X_LED_COUNT) {
        memcpy_P(&led, (&g_sled1734x_leds[index]), sizeof(led));

        if (g_pwm_buffer[led.driver][led.r] == red && g_pwm_buffer[led.driver][led.g] == green && g_pwm_buffer[led.driver][led.b] == blue) {
            return;
        }
        g_pwm_buffer[led.driver][led.r]          = red;
        g_pwm_buffer[led.driver][led.g]          = green;
        g_pwm_buffer[led.driver][led.b]          = blue;
        g_pwm_buffer_update_required[led.driver] = true;
    }
}

void sled1734x_set_color_all(uint8_t red, uint8_t green, uint8_t blue) {
    for (int i = 0; i < SLED1734X_LED_COUNT; i++) {
        sled1734x_set_color(i, red, green, blue);
    }
}

void sled1734x_set_led_control_register(uint8_t index, bool red, bool green, bool blue) {
    sled1734x_led led;
    memcpy_P(&led, (&g_sled1734x_leds[index]), sizeof(led));

    uint8_t control_register_r = (led.r) / 8;
    uint8_t control_register_g = (led.g) / 8;
    uint8_t control_register_b = (led.b) / 8;

    uint8_t bit_r = (led.r) % 8;
    uint8_t bit_g = (led.g) % 8;
    uint8_t bit_b = (led.b) % 8;

    if (red) {
        g_led_control_registers[led.driver][control_register_r] |= (1 << bit_r);
    } else {
        g_led_control_registers[led.driver][control_register_r] &= ~(1 << bit_r);
    }
    if (green) {
        g_led_control_registers[led.driver][control_register_g] |= (1 << bit_g);
    } else {
        g_led_control_registers[led.driver][control_register_g] &= ~(1 << bit_g);
    }
    if (blue) {
        g_led_control_registers[led.driver][control_register_b] |= (1 << bit_b);
    } else {
        g_led_control_registers[led.driver][control_register_b] &= ~(1 << bit_b);
    }

    g_led_control_registers_update_required[led.driver] = true;
}

void sled1734x_update_pwm_buffers(uint8_t addr, uint8_t index) {
    if (g_pwm_buffer_update_required[index]) {
        if (!sled1734x_write_pwm_buffer(addr, g_pwm_buffer[index])) {
            g_led_control_registers_update_required[index] = true;
        }
    }
    g_pwm_buffer_update_required[index] = false;
}

void sled1734x_update_led_control_registers(uint8_t addr, uint8_t index) {
    if (g_led_control_registers_update_required[index]) {
        // select the first frame
        sled1734x_select_page(addr, SLED1734X_COMMAND_FRAME_1);
        for (int i = 0; i < 16; i++) {
            sled1734x_write_register(addr, i, g_led_control_registers[index][i]);
        }
        // select the second frame
        sled1734x_select_page(addr, SLED1734X_COMMAND_FRAME_2);
        for (int i = 0; i < 16; i++) {
            sled1734x_write_register(addr, i, g_led_control_registers[index][i + 16]);
        }
    }
    g_led_control_registers_update_required[index] = false;
}

void sled1734x_flush(void) {
    sled1734x_update_pwm_buffers(SLED1734X_I2C_ADDRESS_1, 0);
#if defined(SLED1734X_I2C_ADDRESS_2)
    sled1734x_update_pwm_buffers(SLED1734X_I2C_ADDRESS_2, 1);
#    if defined(SLED1734X_I2C_ADDRESS_3)
    sled1734x_update_pwm_buffers(SLED1734X_I2C_ADDRESS_3, 2);
#        if defined(SLED1734X_I2C_ADDRESS_4)
    sled1734x_update_pwm_buffers(SLED1734X_I2C_ADDRESS_4, 3);
#        endif
#    endif
#endif
}

void sled1734x_sw_return_normal(uint8_t addr) {
    // Select to function page
    sled1734x_write_register(addr, SLED1734X_REG_COMMAND, SLED1734X_COMMAND_FUNCTION);
    // Setting LED driver to normal mode
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_SOFTWARE_SHUTDOWN, SLED1734X_SOFTWARE_SHUTDOWN_SSD_NORMAL);
}

void sled1734x_sw_shutdown(uint8_t addr) {
    // Select to function page
    sled1734x_write_register(addr, SLED1734X_REG_COMMAND, SLED1734X_COMMAND_FUNCTION);
    // Setting LED driver to shutdown mode
    sled1734x_write_register(addr, SLED1734X_FUNCTION_REG_SOFTWARE_SHUTDOWN, SLED1734X_SOFTWARE_SHUTDOWN_SSD_SHUTDOWN);
}
