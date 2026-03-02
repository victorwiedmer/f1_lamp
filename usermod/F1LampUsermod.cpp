// F1LampUsermod.cpp
// Registration file for the F1 Lamp usermod in modern WLED.
// The class implementation lives entirely in F1LampUsermod.h.
//
// NOTE: Do NOT include wled.h here before F1LampUsermod.h.
//       F1LampUsermod.h intentionally includes the ESP-IDF headers
//       (esp_http_client.h, esp_websocket_client.h) BEFORE wled.h to
//       prevent the HTTP method macro conflict with http_parser.h.

#include "F1LampUsermod.h"

// Static instance — constructed at startup by the linker
static F1LampUsermod f1lamp;

// Register with the WLED usermod manager via linker-section magic
REGISTER_USERMOD(f1lamp);
