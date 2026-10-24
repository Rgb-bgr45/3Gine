// Example native addon. Build as libnative_example.so for arm64-v8a with the NDK, then copy it to
// Documents/3gine/addons/native_example.so
#include "../../app/src/main/cpp/gine_addon.h"
static const GineAPI* api;
static void tick(float dt) { (void)dt; /* runs every frame while playing */ }
extern "C" void gine_addon_init(const GineAPI* a) {
    api = a;
    api->log("native example loaded");
    api->on_update(tick);
}
