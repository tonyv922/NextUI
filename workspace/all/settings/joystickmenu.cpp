#include "joystickmenu.hpp"

extern "C" {
#include "platform.h"
}

#include <string>

#ifdef HAS_JOY_MAP

namespace {

// One row per logical button of an external (Bluetooth) pad.
// A = capture the next external-pad button and bind it to this slot.
// X = restore this slot's factory binding.
// Built-in panel keys travel through raw evdev and never enter this path,
// which is exactly the "not on the device itself" requirement.
class JoyMapItem : public MenuItem
{
    int slot;
    bool capturing = false;

public:
    JoyMapItem(int s, const std::string &buttonName)
        : MenuItem(ListItemType::Generic, buttonName,
                "A: assign, B: cancel, X: default",
                std::vector<std::any>{0}, std::vector<std::string>{""},
                nullptr, nullptr,
                [this]() { PLAT_joystickMapRestoreSlot(slot); }),
          slot(s) {}

    const std::string getLabel() const override
    {
        if (capturing)
            return "press a button/axis";
        int raw = PLAT_joystickMapSlotRaw(slot);
        if (raw < 0)
            return "None";
        if (raw >= JOY_AXIS_BASE) {
            // axis binding: 1000 + axis*2 (+1 = negative direction)
            int v = raw - JOY_AXIS_BASE;
            return "Axis " + std::to_string(v / 2) + (v & 1 ? "-" : "+");
        }
        return "Button " + std::to_string(raw);
    }

    InputReactionHint handleInput(int &dirty) override
    {
        if (capturing)
        {
            // external-pad press this frame? (raw evdev never sets this)
            int raw = PLAT_joystickLastRaw();
            if (raw >= 0)
            {
                PLAT_joystickMapSet(slot, raw);
                capturing = false;
                dirty = 1;
                return NoOp;
            }
            // built-in B cancels; swallowing A keeps a held/repeated pad
            // "A" from re-triggering assignment while we wait
            if (PAD_justPressed(BTN_B))
            {
                capturing = false;
                dirty = 1;
                return NoOp;
            }
            if (PAD_justPressed(BTN_A))
                return NoOp;
            // moving the cursor away ends capture
            if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_DOWN) ||
                PAD_justPressed(BTN_LEFT) || PAD_justPressed(BTN_RIGHT))
            {
                capturing = false;
                dirty = 1;
            }
            // anything else: let the list move
            return Unhandled;
        }

        if (PAD_justPressed(BTN_X))
        {
            PLAT_joystickMapRestoreSlot(slot);
            dirty = 1;
            return NoOp;
        }
        if (PAD_justPressed(BTN_A))
        {
            capturing = true;
            PLAT_joystickLastRaw(); // flush the press that started capture
            dirty = 1;
            return NoOp;
        }
        return Unhandled;
    }
};

struct JoyRow {
    int slot;
    const char *name;
};

} // namespace

MenuList* buildJoystickMenu()
{
    // slot order mirrors the physical layout, most-used first
    static const JoyRow rows[] = {
        {JOY_MAP_A, "A button"},
        {JOY_MAP_B, "B button"},
        {JOY_MAP_X, "X button"},
        {JOY_MAP_Y, "Y button"},
        {JOY_MAP_UP, "D-pad up"},
        {JOY_MAP_DOWN, "D-pad down"},
        {JOY_MAP_LEFT, "D-pad left"},
        {JOY_MAP_RIGHT, "D-pad right"},
        {JOY_MAP_L1, "L1"},
        {JOY_MAP_R1, "R1"},
        {JOY_MAP_L2, "L2"},
        {JOY_MAP_R2, "R2"},
        {JOY_MAP_L3, "L3 (stick click)"},
        {JOY_MAP_R3, "R3 (stick click)"},
        {JOY_MAP_SELECT, "Select"},
        {JOY_MAP_START, "Start"},
        {JOY_MAP_MENU, "Menu"},
        {JOY_MAP_PLUS, "Plus"},
        {JOY_MAP_MINUS, "Minus"},
    };

    std::vector<AbstractMenuItem *> items;
    for (const auto &r : rows)
        items.push_back(new JoyMapItem(r.slot, r.name));

    // stick behaviour toggles (same widget pattern as the BT sample-rate row)
    items.push_back(new MenuItem(ListItemType::Generic, "Stick invert X",
            "Flips the left stick's horizontal direction.",
            {0, 1}, {"Off", "On"},
            []() -> std::any { return PLAT_joystickGetOption(JOY_OPT_INV_X); },
            [](const std::any &v) { PLAT_joystickSetOption(JOY_OPT_INV_X, std::any_cast<int>(v)); }));
    items.push_back(new MenuItem(ListItemType::Generic, "Stick invert Y",
            "Flips the left stick's vertical direction.",
            {0, 1}, {"Off", "On"},
            []() -> std::any { return PLAT_joystickGetOption(JOY_OPT_INV_Y); },
            [](const std::any &v) { PLAT_joystickSetOption(JOY_OPT_INV_Y, std::any_cast<int>(v)); }));
    items.push_back(new MenuItem(ListItemType::Generic, "Stick acts as D-pad",
            "Left stick sends digital directions instead of analog.\nUseful for games/consoles without analog support.",
            {0, 1}, {"Off", "On"},
            []() -> std::any { return PLAT_joystickGetOption(JOY_OPT_STICK_DPAD); },
            [](const std::any &v) { PLAT_joystickSetOption(JOY_OPT_STICK_DPAD, std::any_cast<int>(v)); }));

    items.push_back(new MenuItem{ListItemType::Button, "Reset to defaults",
        "Resets all external-pad buttons to their default assignments.",
        ResetCurrentMenu});

    return new MenuList(MenuItemType::Fixed, "Joystick", std::move(items));
}

#else // !HAS_JOY_MAP

MenuList* buildJoystickMenu()
{
    return nullptr; // platform has no remappable external pad path
}

#endif
