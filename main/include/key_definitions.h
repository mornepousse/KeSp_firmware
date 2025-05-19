#pragma once
#include "tinyusb.h"

//--------------------------------------------------------------------+
// HID KEYCODE
//--------------------------------------------------------------------+
#define K_NONE                        HID_KEY_NONE                        // None
#define K_NO                          HID_KEY_NONE                        // None
#define K_TRNS                        HID_KEY_NONE                        // None
#define K_A                           HID_KEY_A                           // A
#define K_B                           HID_KEY_B                           // B
#define K_C                           HID_KEY_C                           // C
#define K_D                           HID_KEY_D                           // D
#define K_E                           HID_KEY_E                           // E
#define K_F                           HID_KEY_F                           // F
#define K_G                           HID_KEY_G                           // G
#define K_H                           HID_KEY_H                           // H
#define K_I                           HID_KEY_I                           // I
#define K_J                           HID_KEY_J                           // J
#define K_K                           HID_KEY_K                           // K
#define K_L                           HID_KEY_L                           // L
#define K_M                           HID_KEY_M                           // M
#define K_N                           HID_KEY_N                           // N
#define K_O                           HID_KEY_O                           // O
#define K_P                           HID_KEY_P                           // P
#define K_Q                           HID_KEY_Q                           // Q
#define K_R                           HID_KEY_R                           // R
#define K_S                           HID_KEY_S                           // S
#define K_T                           HID_KEY_T                           // T
#define K_U                           HID_KEY_U                           // U
#define K_V                           HID_KEY_V                           // V
#define K_W                           HID_KEY_W                           // W
#define K_X                           HID_KEY_X                           // X
#define K_Y                           HID_KEY_Y                           // Y
#define K_Z                           HID_KEY_Z                           // Z
#define K_1                           HID_KEY_1                           // 1
#define K_2                           HID_KEY_2                           // 2
#define K_3                           HID_KEY_3                           // 3
#define K_4                           HID_KEY_4                           // 4
#define K_5                           HID_KEY_5                           // 5
#define K_6                           HID_KEY_6                           // 6
#define K_7                           HID_KEY_7                           // 7
#define K_8                           HID_KEY_8                           // 8
#define K_9                           HID_KEY_9                           // 9
#define K_0                           HID_KEY_0                           // 0
#define K_ENTER                       HID_KEY_ENTER                       // Enter
#define K_ENT                         HID_KEY_ENTER                       // Enter
#define K_ESCAPE                      HID_KEY_ESCAPE                      // Escape
#define K_ESC	                      HID_KEY_ESCAPE                      // Escape
#define K_BACKSPACE                   HID_KEY_BACKSPACE                   // Backspace
#define K_BSPACE                   	  HID_KEY_BACKSPACE                   // Backspace
#define K_BSPC                   	  HID_KEY_BACKSPACE                   // Backspace
#define K_TAB                         HID_KEY_TAB                         // Tab
#define K_SPACE                       HID_KEY_SPACE                       // Space
#define K_SPC                         HID_KEY_SPACE                       // Space
#define K_MINUS                       HID_KEY_MINUS                       // -
#define K_MIN	                      HID_KEY_MINUS                       // -
#define K_EQUAL                       HID_KEY_EQUAL                       // =
#define K_EQL                         HID_KEY_EQUAL                       // =
#define K_BRACKET_LEFT                HID_KEY_BRACKET_LEFT                // [
#define K_LBRCKT                      HID_KEY_BRACKET_LEFT                // [
#define K_LBRC                        HID_KEY_BRACKET_LEFT                // [
#define K_BRACKET_RIGHT               HID_KEY_BRACKET_RIGHT               // ]
#define K_RBRCKT	                  HID_KEY_BRACKET_RIGHT               // ]
#define K_RBRC  	                  HID_KEY_BRACKET_RIGHT               // ]
#define K_BACKSLASH                   HID_KEY_BACKSLASH                   // " \\ "
#define K_BSLSH                       HID_KEY_BACKSLASH                   // " \\ "
#define K_EUROPE_1                    HID_KEY_EUROPE_1                    // Europe 1
#define K_EUR                         HID_KEY_EUROPE_1                    // Europe 1
#define K_SEMICOLON                   HID_KEY_SEMICOLON                   // ;
#define K_SCOLON                      HID_KEY_SEMICOLON                   // ;
#define K_SCLN                        HID_KEY_SEMICOLON                   // ;
#define K_APOSTROPHE                  HID_KEY_APOSTROPHE                  // '
#define K_APO                         HID_KEY_APOSTROPHE                  // '
#define K_QUOT                        HID_KEY_APOSTROPHE                  // '
#define K_GRAVE                       HID_KEY_GRAVE                       // `
#define K_GRV                         HID_KEY_GRAVE                       // `
#define K_COMMA                       HID_KEY_COMMA                       // ,
#define K_COMM                        HID_KEY_COMMA                       // ,
#define K_COMA                        HID_KEY_COMMA                       // ,
#define K_PERIOD                      HID_KEY_PERIOD                      // .
#define K_DOT                         HID_KEY_PERIOD                      // .
#define K_SLASH                       HID_KEY_SLASH                       // /
#define K_SLSH                        HID_KEY_SLASH                       // /
#define K_CAPS_LOCK                   HID_KEY_CAPS_LOCK                   // Caps Lock
#define K_CLOCK                       HID_KEY_CAPS_LOCK                   // Caps Lock
#define K_F1                          HID_KEY_F1                          // F1
#define K_F2                          HID_KEY_F2                          // F2
#define K_F3                          HID_KEY_F3                          // F3
#define K_F4                          HID_KEY_F4                          // F4
#define K_F5                          HID_KEY_F5                          // F5
#define K_F6                          HID_KEY_F6                          // F6
#define K_F7                          HID_KEY_F7                          // F7
#define K_F8                          HID_KEY_F8                          // F8
#define K_F9                          HID_KEY_F9                          // F9
#define K_F10                         HID_KEY_F10                         // F10
#define K_F11                         HID_KEY_F11                         // F11
#define K_F12                         HID_KEY_F12                         // F12
#define K_PRINT_SCREEN                HID_KEY_PRINT_SCREEN                // Print Screen
#define K_PRNT_SCRN                   HID_KEY_PRINT_SCREEN                // Print Screen
#define K_P_SCRN                      HID_KEY_PRINT_SCREEN                // Print Screen
#define K_SCROLL_LOCK                 HID_KEY_SCROLL_LOCK                 // Scroll Lock
#define K_SCRLL_LCK                   HID_KEY_SCROLL_LOCK                 // Scroll Lock
#define K_PAUSE                       HID_KEY_PAUSE                       // Pause
#define K_PS                          HID_KEY_PAUSE                       // Pause
#define K_INSERT                      HID_KEY_INSERT                      // Insert
#define K_HOME                        HID_KEY_HOME                        // Home
#define K_PAGE_UP                     HID_KEY_PAGE_UP                     // Page Up
#define K_P_UP                        HID_KEY_PAGE_UP                     // Page Up
#define K_DELETE                      HID_KEY_DELETE                      // Delete
#define K_DEL                         HID_KEY_DELETE                      // Delete
#define K_DLT                         HID_KEY_DELETE                      // Delete
#define K_END                         HID_KEY_END                         // End
#define K_PAGE_DOWN                   HID_KEY_PAGE_DOWN                   // Page Down
#define K_P_DOWN                      HID_KEY_PAGE_DOWN                   // Page Down
#define K_ARROW_RIGHT                 HID_KEY_ARROW_RIGHT                 // Right Arrow
#define K_RIGHT                       HID_KEY_ARROW_RIGHT                 // Right Arrow
#define K_ARROW_LEFT                  HID_KEY_ARROW_LEFT                  // Left Arrow
#define K_LEFT                        HID_KEY_ARROW_LEFT                  // Left Arrow
#define K_ARROW_DOWN                  HID_KEY_ARROW_DOWN                  // Down Arrow
#define K_DOWN                        HID_KEY_ARROW_DOWN                  // Down Arrow
#define K_ARROW_UP                    HID_KEY_ARROW_UP                    // Up Arrow
#define K_UP                          HID_KEY_ARROW_UP                    // Up Arrow
#define K_NUM_LOCK                    HID_KEY_NUM_LOCK                    // Num Lock
#define K_NLOCK                       HID_KEY_NUM_LOCK                    // Num Lock
#define K_KEYPAD_DIVIDE               HID_KEY_KEYPAD_DIVIDE               // Keypad /
#define K_KEYPAD_MULTIPLY             HID_KEY_KEYPAD_MULTIPLY             // Keypad *
#define K_KEYPAD_SUBTRACT             HID_KEY_KEYPAD_SUBTRACT             // Keypad -
#define K_KEYPAD_ADD                  HID_KEY_KEYPAD_ADD                  // Keypad +
#define K_KEYPAD_ENTER                HID_KEY_KEYPAD_ENTER                // Keypad Enter
#define K_KEYPAD_1                    HID_KEY_KEYPAD_1                    // Keypad 1
#define K_KEYPAD_2                    HID_KEY_KEYPAD_2                    // Keypad 2
#define K_KEYPAD_3                    HID_KEY_KEYPAD_3                    // Keypad 3
#define K_KEYPAD_4                    HID_KEY_KEYPAD_4                    // Keypad 4
#define K_KEYPAD_5                    HID_KEY_KEYPAD_5                    // Keypad 5
#define K_KEYPAD_6                    HID_KEY_KEYPAD_6                    // Keypad 6
#define K_KEYPAD_7                    HID_KEY_KEYPAD_7                    // Keypad 7
#define K_KEYPAD_8                    HID_KEY_KEYPAD_8                    // Keypad 8
#define K_KEYPAD_9                    HID_KEY_KEYPAD_9                    // Keypad 9
#define K_KEYPAD_0                    HID_KEY_KEYPAD_0                    // Keypad 0
#define K_KEYPAD_DECIMAL              HID_KEY_KEYPAD_DECIMAL              // Keypad .
#define K_EUROPE_2                    HID_KEY_EUROPE_2                    // Europe 2
#define K_APPLICATION                 HID_KEY_APPLICATION                 // Application
#define K_POWER                       HID_KEY_POWER                       // Power
#define K_KEYPAD_EQUAL                HID_KEY_KEYPAD_EQUAL                // Keypad =
#define K_F13                         HID_KEY_F13                         // F13
#define K_F14                         HID_KEY_F14                         // F14
#define K_F15                         HID_KEY_F15                         // F15
#define K_F16                         HID_KEY_F16                         // F16
#define K_F17                         HID_KEY_F17                         // F17
#define K_F18                         HID_KEY_F18                         // F18
#define K_F19                         HID_KEY_F19                         // F19
#define K_F20                         HID_KEY_F20                         // F20
#define K_F21                         HID_KEY_F21                         // F21
#define K_F22                         HID_KEY_F22                         // F22
#define K_F23                         HID_KEY_F23                         // F23
#define K_F24                         HID_KEY_F24                         // F24
#define K_EXECUTE                     HID_KEY_EXECUTE                     // Execute
#define K_HELP                        HID_KEY_HELP                        // Help
#define K_MENU                        HID_KEY_MENU                        // Menu
#define K_SELECT                      HID_KEY_SELECT                      // Select
#define K_STOP                        HID_KEY_STOP                        // Stop
#define K_AGAIN                       HID_KEY_AGAIN                       // Again
#define K_UNDO                        HID_KEY_UNDO                        // Undo
#define K_CUT                         HID_KEY_CUT                         // Cut
#define K_COPY                        HID_KEY_COPY                        // Copy
#define K_PASTE                       HID_KEY_PASTE                       // Paste
#define K_FIND                        HID_KEY_FIND                        // Find
#define K_MUTE                        HID_KEY_MUTE                        // Mute
#define K_VOLUME_UP                   HID_KEY_VOLUME_UP                   // Volume Up
#define K_VOLUME_DOWN                 HID_KEY_VOLUME_DOWN                 // Volume Down
#define K_LOCKING_CAPS_LOCK           HID_KEY_LOCKING_CAPS_LOCK           // Locking Caps Lock
#define K_LOCKING_NUM_LOCK            HID_KEY_LOCKING_NUM_LOCK            // Locking Num Lock
#define K_LOCKING_SCROLL_LOCK         HID_KEY_LOCKING_SCROLL_LOCK         // Locking Scroll Lock
#define K_KEYPAD_COMMA                HID_KEY_KEYPAD_COMMA                // Keypad ,
#define K_KEYPAD_EQUAL_SIGN           HID_KEY_KEYPAD_EQUAL_SIGN           // Keypad =
#define K_KANJI1                      HID_KEY_KANJI1                      // Kanji 1
#define K_KANJI2                      HID_KEY_KANJI2                      // Kanji 2
#define K_KANJI3                      HID_KEY_KANJI3                      // Kanji 3
#define K_KANJI4                      HID_KEY_KANJI4                      // Kanji 4
#define K_KANJI5                      HID_KEY_KANJI5                      // Kanji 5
#define K_KANJI6                      HID_KEY_KANJI6                      // Kanji 6
#define K_KANJI7                      HID_KEY_KANJI7                      // Kanji 7
#define K_KANJI8                      HID_KEY_KANJI8                      // Kanji 8
#define K_KANJI9                      HID_KEY_KANJI9                      // Kanji 9
#define K_LANG1                       HID_KEY_LANG1                       // Lang 1
#define K_LANG2                       HID_KEY_LANG2                       // Lang 2
#define K_LANG3                       HID_KEY_LANG3                       // Lang 3
#define K_LANG4                       HID_KEY_LANG4                       // Lang 4
#define K_LANG5                       HID_KEY_LANG5                       // Lang 5
#define K_LANG6                       HID_KEY_LANG6                       // Lang 6
#define K_LANG7                       HID_KEY_LANG7                       // Lang 7
#define K_LANG8                       HID_KEY_LANG8                       // Lang 8
#define K_LANG9                       HID_KEY_LANG9                       // Lang 9
#define K_ALTERNATE_ERASE             HID_KEY_ALTERNATE_ERASE             // Alternate Erase
#define K_SYSREQ_ATTENTION            HID_KEY_SYSREQ_ATTENTION            // SysReq/Attention
#define K_CANCEL                      HID_KEY_CANCEL                      // Cancel
#define K_CLEAR                       HID_KEY_CLEAR                       // Clear
#define K_PRIOR                       HID_KEY_PRIOR                       // Prior
#define K_RETURN                      HID_KEY_RETURN                      // Return
#define K_SEPARATOR                   HID_KEY_SEPARATOR                   // Separator
#define K_OUT                         HID_KEY_OUT                         // Out
#define K_OPER                        HID_KEY_OPER                        // Oper
#define K_CLEAR_AGAIN                 HID_KEY_CLEAR_AGAIN                 // Clear Again
#define K_CRSEL_PROPS                 HID_KEY_CRSEL_PROPS                 // CrSel/Props
#define K_EXSEL                       HID_KEY_EXSEL                       // ExSel
#define K_KEYPAD_00                   HID_KEY_KEYPAD_00                   // Keypad 00
#define K_KEYPAD_000                  HID_KEY_KEYPAD_000                  // Keypad 000
#define K_THOUSANDS_SEPARATOR         HID_KEY_THOUSANDS_SEPARATOR         // Thousands Separator
#define K_DECIMAL_SEPARATOR           HID_KEY_DECIMAL_SEPARATOR           // Decimal Separator
#define K_CURRENCY_UNIT               HID_KEY_CURRENCY_UNIT               // Currency Unit
#define K_CURRENCY_SUBUNIT            HID_KEY_CURRENCY_SUBUNIT            // Currency Subunit
#define K_KEYPAD_LEFT_PARENTHESIS     HID_KEY_KEYPAD_LEFT_PARENTHESIS     // Keypad (
#define K_L_PARENTHESIS               HID_KEY_KEYPAD_LEFT_PARENTHESIS     // Keypad (
#define K_KEYPAD_RIGHT_PARENTHESIS    HID_KEY_KEYPAD_RIGHT_PARENTHESIS    // Keypad )
#define K_R_PARENTHESIS               HID_KEY_KEYPAD_RIGHT_PARENTHESIS    // Keypad )
#define K_KEYPAD_LEFT_BRACE           HID_KEY_KEYPAD_LEFT_BRACE           // Keypad {
#define K_KEYPAD_RIGHT_BRACE          HID_KEY_KEYPAD_RIGHT_BRACE          // Keypad }
#define K_KEYPAD_TAB                  HID_KEY_KEYPAD_TAB                  // Keypad Tab
#define K_KEYPAD_BACKSPACE            HID_KEY_KEYPAD_BACKSPACE            // Keypad Backspace
#define K_KEYPAD_A                    HID_KEY_KEYPAD_A                    // Keypad A
#define K_KEYPAD_B                    HID_KEY_KEYPAD_B                    // Keypad B
#define K_KEYPAD_C                    HID_KEY_KEYPAD_C                    // Keypad C
#define K_KEYPAD_D                    HID_KEY_KEYPAD_D                    // Keypad D
#define K_KEYPAD_E                    HID_KEY_KEYPAD_E                    // Keypad E
#define K_KEYPAD_F                    HID_KEY_KEYPAD_F                    // Keypad F
#define K_KEYPAD_XOR                  HID_KEY_KEYPAD_XOR                  // Keypad XOR
#define K_KEYPAD_CARET                HID_KEY_KEYPAD_CARET                // Keypad ^
#define K_KEYPAD_PERCENT              HID_KEY_KEYPAD_PERCENT              // Keypad %
#define K_KEYPAD_LESS_THAN            HID_KEY_KEYPAD_LESS_THAN            // Keypad <
#define K_KEYPAD_GREATER_THAN         HID_KEY_KEYPAD_GREATER_THAN         // Keypad >
#define K_KEYPAD_AMPERSAND            HID_KEY_KEYPAD_AMPERSAND            // Keypad &
#define K_KEYPAD_DOUBLE_AMPERSAND     HID_KEY_KEYPAD_DOUBLE_AMPERSAND     // Keypad &&
#define K_KEYPAD_VERTICAL_BAR         HID_KEY_KEYPAD_VERTICAL_BAR         // Keypad |
#define K_KEYPAD_DOUBLE_VERTICAL_BAR  HID_KEY_KEYPAD_DOUBLE_VERTICAL_BAR  // Keypad ||
#define K_KEYPAD_COLON                HID_KEY_KEYPAD_COLON                // Keypad :
#define K_KEYPAD_HASH                 HID_KEY_KEYPAD_HASH                 // Keypad #
#define K_KEYPAD_SPACE                HID_KEY_KEYPAD_SPACE                // Keypad Space
#define K_KEYPAD_AT                   HID_KEY_KEYPAD_AT                   // Keypad @
#define K_KEYPAD_EXCLAMATION          HID_KEY_KEYPAD_EXCLAMATION          // Keypad !
#define K_KEYPAD_MEMORY_STORE         HID_KEY_KEYPAD_MEMORY_STORE         // Keypad Memory Store
#define K_KEYPAD_MEMORY_RECALL        HID_KEY_KEYPAD_MEMORY_RECALL        // Keypad Memory Recall
#define K_KEYPAD_MEMORY_CLEAR         HID_KEY_KEYPAD_MEMORY_CLEAR         // Keypad Memory Clear
#define K_KEYPAD_MEMORY_ADD           HID_KEY_KEYPAD_MEMORY_ADD           // Keypad Memory Add
#define K_KEYPAD_MEMORY_SUBTRACT      HID_KEY_KEYPAD_MEMORY_SUBTRACT      // Keypad Memory Subtract
#define K_KEYPAD_MEMORY_MULTIPLY      HID_KEY_KEYPAD_MEMORY_MULTIPLY      // Keypad Memory Multiply
#define K_KEYPAD_MEMORY_DIVIDE        HID_KEY_KEYPAD_MEMORY_DIVIDE        // Keypad Memory Divide
#define K_KEYPAD_PLUS_MINUS           HID_KEY_KEYPAD_PLUS_MINUS           // Keypad +/-
#define K_KEYPAD_CLEAR                HID_KEY_KEYPAD_CLEAR                // Keypad Clear
#define K_KEYPAD_CLEAR_ENTRY          HID_KEY_KEYPAD_CLEAR_ENTRY          // Keypad Clear Entry
#define K_KEYPAD_BINARY               HID_KEY_KEYPAD_BINARY               // Keypad Binary
#define K_KEYPAD_OCTAL                HID_KEY_KEYPAD_OCTAL                // Keypad Octal
#define K_KEYPAD_DECIMAL_2            HID_KEY_KEYPAD_DECIMAL_2            // Keypad Decimal
#define K_KEYPAD_HEXADECIMAL          HID_KEY_KEYPAD_HEXADECIMAL          // Keypad Hexadecimal
#define K_CONTROL_LEFT                HID_KEY_CONTROL_LEFT                // Left Control
#define K_LCTRL                       HID_KEY_CONTROL_LEFT                // Left Control
#define K_SHIFT_LEFT                  HID_KEY_SHIFT_LEFT                  // Left Shift
#define K_LSHIFT                      HID_KEY_SHIFT_LEFT                  // Left Shift
#define K_ALT_LEFT                    HID_KEY_ALT_LEFT                    // Left Alt
#define K_LALT                        HID_KEY_ALT_LEFT                    // Left Alt
#define K_GUI_LEFT                    HID_KEY_GUI_LEFT                    // Left GUI
#define K_LGUI                        HID_KEY_GUI_LEFT                    // Left GUI
#define K_LWIN                        HID_KEY_GUI_LEFT                    // Left GUI
#define K_CONTROL_RIGHT               HID_KEY_CONTROL_RIGHT               // Right Control
#define K_RCTRL                       HID_KEY_CONTROL_RIGHT               // Right Control
#define K_SHIFT_RIGHT                 HID_KEY_SHIFT_RIGHT                 // Right Shift
#define K_RSHIFT                      HID_KEY_SHIFT_RIGHT                 // Right Shift
#define K_ALT_RIGHT                   HID_KEY_ALT_RIGHT                   // Right Alt
#define K_RALT                        HID_KEY_ALT_RIGHT                   // Right Alt
#define K_GUI_RIGHT                   HID_KEY_GUI_RIGHT                   // Right GUI
#define K_RGUI                        HID_KEY_GUI_RIGHT                   // Right GUI
#define K_RWIN                        HID_KEY_GUI_RIGHT                   // Right GUI


#define K_INT1                        K_KANJI1
#define K_INT2                        K_KANJI2
#define K_INT3                        K_KANJI3
#define K_INT4                        K_KANJI4
#define K_INT5                        K_KANJI5
#define K_INT6                        K_KANJI6
#define K_INT7                        K_KANJI7
#define K_INT8                        K_KANJI8
#define K_BT_SEND                     K_KANJI9
// le dernier : HID_KEY_GUI_RIGHT  0xE7

// Momentary layer
static const uint16_t MO_L1  = 0x0100;
static const uint16_t MO_L2  = 0x0200;
static const uint16_t MO_L3  = 0x0300;
static const uint16_t MO_L4  = 0x0400;
static const uint16_t MO_L5  = 0x0500;
static const uint16_t MO_L6  = 0x0600;
static const uint16_t MO_L7  = 0x0700;
static const uint16_t MO_L8  = 0x0800;
static const uint16_t MO_L9  = 0x0900;
static const uint16_t MO_L10 = 0x0A00;

// Toggle layer
static const uint16_t TO_L1  = 0x0B00;
static const uint16_t TO_L2  = 0x0C00;
static const uint16_t TO_L3  = 0x0D00;
static const uint16_t TO_L4  = 0x0E00;
static const uint16_t TO_L5  = 0x0F00;
static const uint16_t TO_L6  = 0x1000;
static const uint16_t TO_L7  = 0x1100;
static const uint16_t TO_L8  = 0x1200;
static const uint16_t TO_L9  = 0x1300;
static const uint16_t TO_L10 = 0x1400;


// simple macro ex : CTRL +V
#define MACRO_1                       0x1500
#define MACRO_2                       0x1600
#define MACRO_3                       0x1700
#define MACRO_4                       0x1800
#define MACRO_5                       0x1900
#define MACRO_6                       0x1A00
#define MACRO_7                       0x1B00
#define MACRO_8                       0x1C00
#define MACRO_9                       0x1D00
#define MACRO_10                      0x1E00
#define MACRO_11                      0x1F00
#define MACRO_12                      0x2000
#define MACRO_13                      0x2100
#define MACRO_14                      0x2200
#define MACRO_15                      0x2300
#define MACRO_16                      0x2400
#define MACRO_17                      0x2500
#define MACRO_18                      0x2600
#define MACRO_19                      0x2700
#define MACRO_20                      0x2800

// Extra Macros
#define EXTRA_MACRO_1                 0x0100
#define EXTRA_MACRO_2                 0x0100
#define EXTRA_MACRO_3                 0x0100
#define EXTRA_MACRO_4                 0x0100
#define EXTRA_MACRO_5                 0x0100
#define EXTRA_MACRO_6                 0x0100
#define EXTRA_MACRO_7                 0x0100
#define EXTRA_MACRO_8                 0x0100
#define EXTRA_MACRO_9                 0x0100
#define EXTRA_MACRO_10                0x0100
#define EXTRA_MACRO_11                0x0100
#define EXTRA_MACRO_12                0x0100
#define EXTRA_MACRO_13                0x0100
#define EXTRA_MACRO_14                0x0100
#define EXTRA_MACRO_15                0x0100
#define EXTRA_MACRO_16                0x0100
#define EXTRA_MACRO_17                0x0100
#define EXTRA_MACRO_18                0x0100
#define EXTRA_MACRO_19                0x0100
#define EXTRA_MACRO_20                0x0100

// NA codes
#define NA51                          0x0100
#define NA52                          0x0100
#define NA53                          0x0100
#define NA54                          0x0100
#define NA55                          0x0100
#define NA56                          0x0100
#define NA57                          0x0100
#define NA58                          0x0100
#define NA59                          0x0100
#define NA60                          0x0100
#define NA61                          0x0100
#define NA62                          0x0100
#define NA63                          0x0100
#define NA64                          0x0100
#define NA65                          0x0100
#define NA66                          0x0100
#define NA67                          0x0100
#define NA68                          0x0100
#define NA69                          0x0100
#define NA70                          0x0100
#define NA71                          0x0100
#define NA72                          0x0100
#define NA73                          0x0100
#define NA74                          0x0100
#define NA75                          0x0100
#define NA76                          0x0100
#define NA77                          0x0100
#define NA78                          0x0100
#define NA79                          0x0100
#define NA80                          0x0100
#define NA81                          0x0100
#define NA82                          0x0100
#define NA83                          0x0100
#define NA84                          0x0100
#define NA85                          0x0100
#define NA86                          0x0100
#define NA87                          0x0100
#define NA88                          0x0100
#define NA89                          0x0100
#define NA90                          0x0100
#define NA91                          0x0100
#define NA92                          0x0100
#define NA93                          0x0100
#define NA94                          0x0100
#define NA95                          0x0100
#define NA96                          0x0100
#define NA97                          0x0100
#define NA98                          0x0100
#define NA99                          0x0100
#define NA100                         0x0100
