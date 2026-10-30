
extern "C" bool Ultron_RegisterAction(
    const char* name,
    void (*callback)());

extern "C" bool Ultron_SetElementProperty(
    const char* element_id,
    const char* property_name,
    const char* value);

extern "C" bool Ultron_SetElementText(
    const char* element_id,
    const char* text);

namespace {

constexpr const char* ROBOTO_FLEX = "\"Roboto Flex\"";
constexpr const char* FALLBACK_FONT = "sans-serif";

bool roboto_flex_active = true;

bool ForceVisible()
{
    const bool display = Ultron_SetElementProperty(
        "font-sample", "display", "block");

    const bool visibility = Ultron_SetElementProperty(
        "font-sample", "visibility", "visible");

    const bool opacity = Ultron_SetElementProperty(
        "font-sample", "opacity", "1");

    return display && visibility && opacity;
}

void ToggleFont()
{
    roboto_flex_active = !roboto_flex_active;

    const char* font = roboto_flex_active
        ? ROBOTO_FLEX
        : FALLBACK_FONT;

    const bool applied = Ultron_SetElementProperty(
        "font-sample",
        "font-family",
        font);

    const bool visible = ForceVisible();

    if (applied && visible)
    {
        Ultron_SetElementText(
            "font-status",
            roboto_flex_active
                ? "Current Font: Roboto Flex"
                : "Current Font: sans-serif");
    }
    else
    {
        Ultron_SetElementText(
            "font-status",
            "Font or visibility update failed!");
    }
}

void ShowFont()
{
    if (ForceVisible())
    {
        Ultron_SetElementText(
            "font-status",
            "Text visibility restored!");
    }
    else
    {
        Ultron_SetElementText(
            "font-status",
            "Visibility update failed!");
    }
}

struct FontSwitcherRegistration
{
    FontSwitcherRegistration()
    {
        Ultron_RegisterAction(
            "toggle_font", &ToggleFont);

        Ultron_RegisterAction(
            "show_font", &ShowFont);
    }
};

[[maybe_unused]]
FontSwitcherRegistration registration;

}
