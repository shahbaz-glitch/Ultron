
#include <stdbool.h>

// Match these declarations with the existing Ultron public API.
extern "C" void Ultron_RegisterAction(
    const char* action_name,
    void (*callback)()
);

extern "C" bool Ultron_ShowDocument(
    const char* document_name,
    const char* rml_path
);

namespace
{
    // The square screen is currently visible at startup.
    bool g_next_is_home = true;

    void ToggleRing()
    {
        bool switched = false;

        if (g_next_is_home)
        {
            switched = Ultron_ShowDocument(
                "home",
                "ui/home.rml"
            );
        }
        else
        {
            switched = Ultron_ShowDocument(
                "square_ring",
                "ui/square_ring.rml"
            );
        }

        // Update the toggle only after a successful switch.
        if (switched)
        {
            g_next_is_home = !g_next_is_home;
        }
    }

    struct RegisterSwitchAction
    {
        RegisterSwitchAction()
        {
            Ultron_RegisterAction(
                "toggle_ring",
                &ToggleRing
            );
        }
    };

    const RegisterSwitchAction g_register_switch_action;
}
