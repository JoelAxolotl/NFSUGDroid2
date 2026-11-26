package com.rea.nfsug2;

import android.view.KeyEvent;

/** Android key codes to DirectInput (DIK_*) scan codes for physical keyboards. */
final class KeyMap {
    private KeyMap() {}

    private static final int[] TABLE = new int[KeyEvent.getMaxKeyCode() + 1];

    private static void put(int key, int dik) {
        TABLE[key] = dik;
    }

    static {
        put(KeyEvent.KEYCODE_ESCAPE, 0x01);
        int[] digits = {0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
        for (int i = 0; i < 10; i++) put(KeyEvent.KEYCODE_0 + i, digits[i]);
        put(KeyEvent.KEYCODE_MINUS, 0x0C);
        put(KeyEvent.KEYCODE_EQUALS, 0x0D);
        put(KeyEvent.KEYCODE_DEL, 0x0E);
        put(KeyEvent.KEYCODE_TAB, 0x0F);
        String row1 = "QWERTYUIOP";
        int[] row1c = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
        String row2 = "ASDFGHJKL";
        int[] row2c = {0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26};
        String row3 = "ZXCVBNM";
        int[] row3c = {0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32};
        for (int i = 0; i < row1.length(); i++) put(KeyEvent.KEYCODE_A + (row1.charAt(i) - 'A'), row1c[i]);
        for (int i = 0; i < row2.length(); i++) put(KeyEvent.KEYCODE_A + (row2.charAt(i) - 'A'), row2c[i]);
        for (int i = 0; i < row3.length(); i++) put(KeyEvent.KEYCODE_A + (row3.charAt(i) - 'A'), row3c[i]);
        put(KeyEvent.KEYCODE_LEFT_BRACKET, 0x1A);
        put(KeyEvent.KEYCODE_RIGHT_BRACKET, 0x1B);
        put(KeyEvent.KEYCODE_ENTER, 0x1C);
        put(KeyEvent.KEYCODE_CTRL_LEFT, 0x1D);
        put(KeyEvent.KEYCODE_SEMICOLON, 0x27);
        put(KeyEvent.KEYCODE_APOSTROPHE, 0x28);
        put(KeyEvent.KEYCODE_GRAVE, 0x29);
        put(KeyEvent.KEYCODE_SHIFT_LEFT, 0x2A);
        put(KeyEvent.KEYCODE_BACKSLASH, 0x2B);
        put(KeyEvent.KEYCODE_COMMA, 0x33);
        put(KeyEvent.KEYCODE_PERIOD, 0x34);
        put(KeyEvent.KEYCODE_SLASH, 0x35);
        put(KeyEvent.KEYCODE_SHIFT_RIGHT, 0x36);
        put(KeyEvent.KEYCODE_ALT_LEFT, 0x38);
        put(KeyEvent.KEYCODE_SPACE, 0x39);
        for (int i = 0; i < 10; i++) put(KeyEvent.KEYCODE_F1 + i, 0x3B + i);
        put(KeyEvent.KEYCODE_F11, 0x57);
        put(KeyEvent.KEYCODE_F12, 0x58);
        put(KeyEvent.KEYCODE_CTRL_RIGHT, 0x9D);
        put(KeyEvent.KEYCODE_ALT_RIGHT, 0xB8);
        put(KeyEvent.KEYCODE_MOVE_HOME, 0xC7);
        put(KeyEvent.KEYCODE_DPAD_UP, 0xC8);
        put(KeyEvent.KEYCODE_PAGE_UP, 0xC9);
        put(KeyEvent.KEYCODE_DPAD_LEFT, 0xCB);
        put(KeyEvent.KEYCODE_DPAD_RIGHT, 0xCD);
        put(KeyEvent.KEYCODE_MOVE_END, 0xCF);
        put(KeyEvent.KEYCODE_DPAD_DOWN, 0xD0);
        put(KeyEvent.KEYCODE_PAGE_DOWN, 0xD1);
        put(KeyEvent.KEYCODE_INSERT, 0xD2);
        put(KeyEvent.KEYCODE_FORWARD_DEL, 0xD3);
    }

    static int toDik(int keyCode) {
        return keyCode >= 0 && keyCode < TABLE.length ? TABLE[keyCode] : 0;
    }
}
