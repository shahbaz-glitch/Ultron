#include "ultron_api.h"

namespace
{
    void ShowRedCircle() noexcept
    {
        (void)Ultron_SetElementClass(
            "red-circle",
            "visible",
            true);
    }

    [[maybe_unused]]
    const bool registered =
        Ultron_RegisterAction(
            "show-red-circle",
            &ShowRedCircle);
}
