/*
 * Generic (descriptor based) controller detection
 * See controller_detect.h for the rationale.
 */

#include "controller_detect.h"
#include <string.h>

#define USB_CLASS_VENDOR_SPECIFIC   0xFF

// Xbox 360 compatible (XInput) interface signature
#define XB360_IF_SUBCLASS           0x5D
#define XB360_IF_PROTOCOL           0x01    // 0x81 would be the wireless receiver (not supported)

// Xbox One / GIP interface signature
#define XBONE_IF_SUBCLASS           0x47
#define XBONE_IF_PROTOCOL           0xD0

#define EP_DIR_IN                   0x80
#define EP_XFER_MASK                0x03
#define EP_XFER_INTERRUPT           0x03

int controller_detect_by_descriptor(libusb_device* dev, DetectedController* out) {
    if (dev == NULL || out == NULL) {
        return 0;
    }

    struct libusb_config_descriptor* cfg = NULL;
    if (usbd_get_active_config_descriptor(dev, &cfg) != 0 || cfg == NULL) {
        return 0;
    }

    int found = 0;

    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface* intf = &cfg->interface[i];

        for (int a = 0; a < intf->num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor* alt = &intf->altsetting[a];
            ControllerType type = CONTROLLER_NONE;

            if (alt->bInterfaceClass != USB_CLASS_VENDOR_SPECIFIC) {
                continue;
            }

            if (alt->bInterfaceSubClass == XB360_IF_SUBCLASS &&
                alt->bInterfaceProtocol == XB360_IF_PROTOCOL) {
                type = CONTROLLER_XBOX360;
            } else if (alt->bInterfaceSubClass == XBONE_IF_SUBCLASS &&
                       alt->bInterfaceProtocol == XBONE_IF_PROTOCOL) {
                type = CONTROLLER_XBOXONE;
            } else {
                continue;
            }

            // First interrupt IN and OUT endpoints of this alt setting
            uint8_t ep_in = 0;
            uint8_t ep_out = 0;
            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor* ep = &alt->endpoint[k];
                if ((ep->bmAttributes & EP_XFER_MASK) != EP_XFER_INTERRUPT) {
                    continue;
                }
                if (ep->bEndpointAddress & EP_DIR_IN) {
                    if (ep_in == 0) ep_in = ep->bEndpointAddress;
                } else {
                    if (ep_out == 0) ep_out = ep->bEndpointAddress;
                }
            }

            // An alt setting without an input endpoint is of no use to us
            // (e.g. Xbox One audio interface alt 0) - keep looking.
            if (ep_in == 0) {
                continue;
            }

            memset(out, 0, sizeof(*out));
            out->type          = type;
            out->interface_num = alt->bInterfaceNumber;
            out->alt_setting   = alt->bAlternateSetting;
            out->ep_in         = ep_in;
            out->ep_out        = ep_out;
            found = 1;
        }
    }

    usbd_free_config_descriptor(cfg);
    return found;
}
