// Keysyms X11 usados na sintaxe de hotkeys. No Linux vêm de <X11/keysym.h>;
// no Windows não há Xlib, então os valores (os mesmos de keysymdef.h) ficam
// aqui. A config e o protocolo usam keysyms nas duas plataformas.
#pragma once

#ifndef _WIN32
#include <X11/keysym.h>
#else

#define XK_space 0x0020
#define XK_apostrophe 0x0027
#define XK_comma 0x002c
#define XK_minus 0x002d
#define XK_period 0x002e
#define XK_slash 0x002f
#define XK_0 0x0030
#define XK_9 0x0039
#define XK_semicolon 0x003b
#define XK_equal 0x003d
#define XK_bracketleft 0x005b
#define XK_backslash 0x005c
#define XK_bracketright 0x005d
#define XK_grave 0x0060
#define XK_a 0x0061
#define XK_z 0x007a
#define XK_BackSpace 0xff08
#define XK_Tab 0xff09
#define XK_Return 0xff0d
#define XK_Pause 0xff13
#define XK_Scroll_Lock 0xff14
#define XK_Escape 0xff1b
#define XK_Home 0xff50
#define XK_Left 0xff51
#define XK_Up 0xff52
#define XK_Right 0xff53
#define XK_Down 0xff54
#define XK_Prior 0xff55
#define XK_Next 0xff56
#define XK_End 0xff57
#define XK_Print 0xff61
#define XK_Insert 0xff63
#define XK_Menu 0xff67
#define XK_KP_Enter 0xff8d
#define XK_KP_Multiply 0xffaa
#define XK_KP_Add 0xffab
#define XK_KP_Subtract 0xffad
#define XK_KP_Decimal 0xffae
#define XK_KP_Divide 0xffaf
#define XK_KP_0 0xffb0
#define XK_KP_9 0xffb9
#define XK_F1 0xffbe
#define XK_F24 0xffd5
#define XK_Shift_L 0xffe1
#define XK_Shift_R 0xffe2
#define XK_Control_L 0xffe3
#define XK_Control_R 0xffe4
#define XK_Alt_L 0xffe9
#define XK_Alt_R 0xffea
#define XK_Super_L 0xffeb
#define XK_Super_R 0xffec
#define XK_Delete 0xffff

#endif
