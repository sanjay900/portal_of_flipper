#pragma once

#include <furi_hal.h>
#include <furi_hal_version.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>

#include "usb.h"
#include "usb_hid.h"
#include "virtual_portal.h"

#define HID_REPORT_TYPE_INPUT   1
#define HID_REPORT_TYPE_OUTPUT  2
#define HID_REPORT_TYPE_FEATURE 3

typedef struct PoFUsb PoFUsb;

PoFUsb* pof_usb_start(VirtualPortal* virtual_portal);
void pof_usb_stop(PoFUsb* pof);

/*descriptor type*/
typedef enum {
    PoFDescriptorTypeDevice = 0x01,
    PoFDescriptorTypeConfig = 0x02,
    PoFDescriptorTypeString = 0x03,
    PoFDescriptorTypeInterface = 0x04,
    PoFDescriptorTypeEndpoint = 0x05,
} PoFDescriptorType;

/*endpoint direction*/
typedef enum {
    PoFEndpointIn = 0x80,
    PoFEndpointOut = 0x00,
} PoFEndpointDirection;

/*endpoint type*/
typedef enum {
    PoFEndpointTypeCtrl = 0x00,
    PoFEndpointTypeIso = 0x01,
    PoFEndpointTypeBulk = 0x02,
    PoFEndpointTypeIntr = 0x03,
} PoFEndpointType;

/*control request type*/
typedef enum {
    PoFControlTypeStandard = (0 << 5),
    PoFControlTypeClass = (1 << 5),
    PoFControlTypeVendor = (2 << 5),
    PoFControlTypeReserved = (3 << 5),
} PoFControlType;

/*control request recipient*/
typedef enum {
    PoFControlRecipientDevice = 0,
    PoFControlRecipientInterface = 1,
    PoFControlRecipientEndpoint = 2,
    PoFControlRecipientOther = 3,
} PoFControlRecipient;

/*control request direction*/
typedef enum {
    PoFControlOut = 0x00,
    PoFControlIn = 0x80,
} PoFControlDirection;

/*endpoint address mask*/
typedef enum {
    PoFEndpointAddrMask = 0x0f,
    PoFEndpointDirMask = 0x80,
    PoFEndpointTransferTypeMask = 0x03,
    PoFCtrlDirMask = 0x80,
} PoFEndpointMask;

/* USB control requests */
typedef enum {
    PoFControlRequestsOut = (PoFControlTypeVendor | PoFControlRecipientDevice | PoFControlOut),
    PoFControlRequestsIn = (PoFControlTypeVendor | PoFControlRecipientDevice | PoFControlIn),
} PoFControlRequests;
