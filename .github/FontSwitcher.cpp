
#include <cstddef>

extern "C" bool Ultron_RegisterAction(
    const char* name, void (*callback)());

extern "C" bool Ultron_SetElementProperty(
    const char* id,
    const char* property,
    const char* value);

namespace {

constexpr const char* FONTS[] = {
    "Roboto Flex",
    "OTHER_FONT_FAMILY"
};

std::size_t current_font = 0;

void ToggleFont()
{
    current_font = 1 - current_font;

    Ultron_SetElementProperty(
        "font-sample",
        "font-family",
        FONTS[current_font]);
}

struct FontSwitcher
{
    FontSwitcher()
    {
        Ultron_RegisterAction("toggle_font", &ToggleFont);
    }
};

FontSwitcher font_switcher;

}
