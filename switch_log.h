#pragma once

#include <string>

// Owned by the window receiving WM_SWITCH_LOG. The event is formatted on
// the network thread, but only the window knows which language to display.
struct SwitchLogMessage {
    std::wstring english;
    std::wstring chinese;
};
