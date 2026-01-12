/*
 * Copyright (C) 2021, 2024 nukeykt
 * LCD emulator for JV-880 (24x2 character display)
 * Based on jv880_juce implementation
 */
#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>

struct MCU;

#define LCD_COLS 24
#define LCD_ROWS 2

struct LCD {
    MCU *mcu;

    uint32_t LCD_DL, LCD_N, LCD_F, LCD_D, LCD_C, LCD_B, LCD_ID, LCD_S;
    uint32_t LCD_DD_RAM, LCD_AC, LCD_CG_RAM;
    uint32_t LCD_RAM_MODE = 0;
    uint8_t LCD_Data[80];
    uint8_t LCD_CG[64];

    uint8_t lcd_enable = 1;

    LCD() : mcu(nullptr) {
        LCD_Init();
    }

    LCD(MCU *m) : mcu(m) {
        LCD_Init();
    }

    void LCD_Init() {
        memset(LCD_Data, 0x20, sizeof(LCD_Data));  // Fill with spaces
        memset(LCD_CG, 0, sizeof(LCD_CG));
        LCD_DL = LCD_N = LCD_F = LCD_D = LCD_C = LCD_B = LCD_ID = LCD_S = 0;
        LCD_DD_RAM = LCD_AC = LCD_CG_RAM = 0;
        LCD_RAM_MODE = 0;
        lcd_enable = 1;
    }

    void LCD_Enable(uint32_t enable) {
        lcd_enable = enable;
    }

    void LCD_Write(uint32_t address, uint8_t data) {
        if (address == 0) {
            // Command register
            if ((data & 0xe0) == 0x20) {
                // Function set
                LCD_DL = (data & 0x10) != 0;
                LCD_N = (data & 0x8) != 0;
                LCD_F = (data & 0x4) != 0;
            } else if ((data & 0xf8) == 0x8) {
                // Display on/off control
                LCD_D = (data & 0x4) != 0;
                LCD_C = (data & 0x2) != 0;
                LCD_B = (data & 0x1) != 0;
            } else if ((data & 0xff) == 0x01) {
                // Clear display
                LCD_DD_RAM = 0;
                LCD_ID = 1;
                memset(LCD_Data, 0x20, sizeof(LCD_Data));
            } else if ((data & 0xff) == 0x02) {
                // Return home
                LCD_DD_RAM = 0;
            } else if ((data & 0xfc) == 0x04) {
                // Entry mode set
                LCD_ID = (data & 0x2) != 0;
                LCD_S = (data & 0x1) != 0;
            } else if ((data & 0xc0) == 0x40) {
                // Set CGRAM address
                LCD_CG_RAM = (data & 0x3f);
                LCD_RAM_MODE = 0;
            } else if ((data & 0x80) == 0x80) {
                // Set DDRAM address
                LCD_DD_RAM = (data & 0x7f);
                LCD_RAM_MODE = 1;
            }
        } else {
            // Data register
            if (!LCD_RAM_MODE) {
                // CGRAM write
                LCD_CG[LCD_CG_RAM] = data & 0x1f;
                if (LCD_ID)
                    LCD_CG_RAM++;
                else
                    LCD_CG_RAM--;
                LCD_CG_RAM &= 0x3f;
            } else {
                // DDRAM write - character data
                if (LCD_N) {
                    // 2-line mode
                    if (LCD_DD_RAM & 0x40) {
                        if ((LCD_DD_RAM & 0x3f) < 40)
                            LCD_Data[(LCD_DD_RAM & 0x3f) + 40] = data;
                    } else {
                        if ((LCD_DD_RAM & 0x3f) < 40)
                            LCD_Data[LCD_DD_RAM & 0x3f] = data;
                    }
                } else {
                    // 1-line mode
                    if (LCD_DD_RAM < 80)
                        LCD_Data[LCD_DD_RAM] = data;
                }
                if (LCD_ID)
                    LCD_DD_RAM++;
                else
                    LCD_DD_RAM--;
                LCD_DD_RAM &= 0x7f;
            }
        }
    }

    unsigned char LCD_Read(unsigned int address) {
        (void)address;
        return 0;
    }

    /* Get line text for JV-880 (24x2 display) */
    /* Line 0: LCD_Data[0..23], Line 1: LCD_Data[40..63] */
    const char* GetLine(int row) const {
        static char line_buf[LCD_COLS + 1];
        int offset = (row == 0) ? 0 : 40;
        for (int i = 0; i < LCD_COLS; i++) {
            uint8_t ch = LCD_Data[offset + i];
            // Convert to printable ASCII
            if (ch >= 0x20 && ch < 0x7f)
                line_buf[i] = ch;
            else
                line_buf[i] = ' ';
        }
        line_buf[LCD_COLS] = '\0';
        return line_buf;
    }

    /* Get cursor visibility (LCD_C flag) */
    bool IsCursorVisible() const {
        return LCD_C != 0;
    }

    /* Get cursor row (0 or 1) */
    int GetCursorRow() const {
        return (LCD_DD_RAM & 0x40) ? 1 : 0;
    }

    /* Get cursor column (0-23) */
    int GetCursorCol() const {
        int col = LCD_DD_RAM & 0x3F;
        return (col < LCD_COLS) ? col : -1;
    }
};
