#include "portal_of_flipper_i.h"

#include <furi.h>

#define TAG "PoF"

void pof_start(PoFApp* app) {
    furi_assert(app);

    app->pof_usb = pof_usb_start_xbox_360(app->virtual_portal);
    // app->pof_usb = pof_usb_start(app->virtual_portal);
}

void pof_stop(PoFApp* app) {
    furi_assert(app);

    pof_usb_stop(app->pof_usb);
}
