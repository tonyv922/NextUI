#pragma once

#include "menu.hpp"

// "Joystick" settings submenu: remaps buttons of the *external*
// (Bluetooth) pad only. Built-in panel keys go through raw evdev and
// are deliberately untouched, as is the pad hat/axes in v1.
//
// One row per remappable logical button. A = capture the next external-pad
// button press and bind it, X = reset that row to its factory default,
// bottom row = reset all. Persists to USERDATA_PATH "/joymap.txt".
MenuList* buildJoystickMenu();
