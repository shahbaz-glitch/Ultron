using UltronActionCallback = void (*)();

extern "C" bool Ultron_RegisterAction(
    const char* name,
    UltronActionCallback callback);

extern "C" bool Ultron_SetElementClass(
    const char* element_id,
    const char* class_name,
    bool enabled);

namespace
{
    void ChangeDotColour()
    {
        static bool green = false;
        green = !green;

        Ultron_SetElementClass("dot", "green", green);
    }

    [[maybe_unused]]
    const bool registered =
        Ultron_RegisterAction(
            "change-dot",
            &ChangeDotColour);
}
