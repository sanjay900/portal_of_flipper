#include "pof_usb.h"

#define TAG "POF USB"

#define HID_INTERVAL 1

#define USB_EP0_SIZE 8

#define POF_USB_VID (0x1430)
#define POF_USB_PID (0x0150)
#define POF_USB_PID_X360 (0x1F17)

#define POF_USB_EP_IN  (0x81)
#define POF_USB_EP_OUT (0x02)
#define POF_USB_X360_AUDIO_EP_IN1  (0x83)
#define POF_USB_X360_AUDIO_EP_OUT1  (0x04)
#define POF_USB_X360_AUDIO_EP_IN2  (0x85)
#define POF_USB_X360_AUDIO_EP_OUT2  (0x06)
#define POF_USB_X360_PLUGIN_MODULE_EP_IN  (0x87)

#define POF_USB_EP_IN_SIZE  (64UL)
#define POF_USB_EP_OUT_SIZE (64UL)

#define POF_USB_RX_MAX_SIZE (POF_USB_EP_OUT_SIZE)
#define POF_USB_TX_MAX_SIZE (POF_USB_EP_IN_SIZE)

#define POF_USB_ACTUAL_OUTPUT_SIZE 0x20

static const struct usb_string_descriptor dev_manuf_desc =
    USB_ARRAY_DESC(0x41, 0x63, 0x74, 0x69, 0x76, 0x69, 0x73, 0x69, 0x6f, 0x6e, 0x00);
static const struct usb_string_descriptor dev_product_desc =
    USB_ARRAY_DESC(0x53, 0x70, 0x79, 0x72, 0x6f, 0x20, 0x50, 0x6f, 0x72, 0x74, 0x61, 0x00);
static const struct usb_string_descriptor dev_security_desc = 
    USB_ARRAY_DESC('X', 'b', 'o', 'x', ' ', 'S', 'e', 'c', 'u', 'r', 'i', 't', 'y',
    ' ', 'M', 'e', 't', 'h', 'o', 'd', ' ', '3', ',', ' ',
    'V', 'e', 'r', 's', 'i', 'o', 'n', ' ', '1', '.', '0', '0', ',',
    ' ', 0xa9, ' ', '2', '0', '0', '5', ' ',
    'M', 'i', 'c', 'r', 'o', 's', 'o', 'f', 't', ' ',
    'C', 'o', 'r', 'p', 'o', 'r', 'a', 't', 'i', 'o', 'n', '.', ' ',
    'A', 'l', 'l', ' ', 'r', 'i', 'g', 'h', 't', 's', ' ',
    'r', 'e', 's', 'e', 'r', 'v', 'e', 'd', '.');

static const uint8_t hid_report_desc[] = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x19,
                                          0x01, 0x29, 0x40, 0x15, 0x00, 0x26, 0xFF, 0x00,
                                          0x75, 0x08, 0x95, 0x20, 0x81, 0x00, 0x19, 0x01,
                                          0x29, 0x40, 0x91, 0x00, 0xC0};

static usbd_respond pof_usb_ep_config(usbd_device* dev, uint8_t cfg);
static usbd_respond
    pof_hid_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback);
static void pof_usb_send(usbd_device* dev, uint8_t* buf, uint16_t len);
static int32_t pof_usb_receive(usbd_device* dev, uint8_t* buf, uint16_t max_len);

typedef enum {
    EventExit = (1 << 0),
    EventReset = (1 << 1),
    EventRx = (1 << 2),
    EventTx = (1 << 3),
    EventTxComplete = (1 << 4),
    EventResetSio = (1 << 5),
    EventTxImmediate = (1 << 6),

    EventAll = EventExit | EventReset | EventRx | EventTx | EventTxComplete | EventResetSio |
               EventTxImmediate,
} PoFEvent;

struct PoFUsb {
    FuriHalUsbInterface usb;
    FuriHalUsbInterface* usb_prev;

    FuriThread* thread;
    usbd_device* dev;
    VirtualPortal* virtual_portal;
    uint8_t data_recvest[8];
    uint16_t data_recvest_len;

    bool tx_complete;
    bool tx_immediate;

    uint8_t dataAvailable;
    uint8_t data[POF_USB_RX_MAX_SIZE];

    uint8_t tx_data[POF_USB_TX_MAX_SIZE];
};

static PoFUsb* pof_cur = NULL;

static int32_t pof_thread_worker(void* context) {
    PoFUsb* pof_usb = context;
    usbd_device* dev = pof_usb->dev;
    VirtualPortal* virtual_portal = pof_usb->virtual_portal;
    UNUSED(dev);

    uint32_t len_data = 0;
    uint8_t tx_data[POF_USB_TX_MAX_SIZE] = {0};
    uint32_t timeout = 100; // FuriWaitForever; //ms

    while(true) {
        uint32_t flags = furi_thread_flags_wait(EventAll, FuriFlagWaitAny, timeout);
        if(flags & EventRx) { //fast flag
            UNUSED(pof_usb_receive);

            if(virtual_portal->speaker) {
                uint8_t buf[POF_USB_RX_MAX_SIZE];
                len_data = pof_usb_receive(dev, buf, POF_USB_RX_MAX_SIZE);
                // https://github.com/xMasterX/all-the-plugins/blob/dev/base_pack/wav_player/wav_player_hal.c
                if(len_data > 0) {
                    /*
                    FURI_LOG_RAW_I("pof_usb_receive: ");
                    for(uint32_t i = 0; i < len_data; i++) {
                        FURI_LOG_RAW_I("%02x", buf[i]);
                    }
                    FURI_LOG_RAW_I("\r\n");
                    */
                }
            }
            if (virtual_portal->type == PoFXbox360) {
                pof_usb_receive(dev, pof_usb->data, POF_USB_RX_MAX_SIZE);
            }

            if(pof_usb->dataAvailable > 0) {
                memset(tx_data, 0, sizeof(tx_data));
                if (pof_usb ->virtual_portal->type == PoFXbox360) {
                    // prepend packet with xinput header
                    int send_len =
                        virtual_portal_process_message(virtual_portal, pof_usb->data + 2, tx_data + 2);
                    if(send_len > 0) {
                        tx_data[0] = 0x0b;
                        tx_data[1] = 0x14;
                        pof_usb_send(dev, tx_data, POF_USB_ACTUAL_OUTPUT_SIZE);
                    }
                }
                if (pof_usb ->virtual_portal->type == PoFHID) {
                    int send_len =
                        virtual_portal_process_message(virtual_portal, pof_usb->data, tx_data);
                    if(send_len > 0) {
                        pof_usb_send(dev, tx_data, POF_USB_ACTUAL_OUTPUT_SIZE);
                    }
                }
                pof_usb->dataAvailable = 0;
            }

            flags &= ~EventRx; // clear flag
        }

        if(flags) {
            if(flags & EventResetSio) {
            }
            if(flags & EventTxComplete) {
                pof_usb->tx_complete = true;
            }

            if(flags & EventTxImmediate) {
                pof_usb->tx_immediate = true;
                if(pof_usb->tx_complete) {
                    flags |= EventTx;
                }
            }

            if(flags & EventTx) {
                pof_usb->tx_complete = false;
                pof_usb->tx_immediate = false;
            }

            if(flags & EventExit) {
                FURI_LOG_I(TAG, "exit");
                break;
            }
        }

        if(flags == (uint32_t)FuriFlagErrorISR) { // timeout
            memset(tx_data, 0, sizeof(tx_data));
            if (pof_usb ->virtual_portal->type == PoFXbox360) {
                len_data = virtual_portal_send_status(virtual_portal, tx_data + 2);
                if(len_data > 0) {
                    tx_data[0] = 0x0b;
                    tx_data[1] = 0x14;
                    pof_usb_send(dev, tx_data, POF_USB_ACTUAL_OUTPUT_SIZE);
                }
            }
            if (pof_usb ->virtual_portal->type == PoFHID) {
                len_data = virtual_portal_send_status(virtual_portal, tx_data);
                if(len_data > 0) {
                    pof_usb_send(dev, tx_data, POF_USB_ACTUAL_OUTPUT_SIZE);
                }
            }
        }
    }

    return 0;
}

static void pof_usb_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx) {
    UNUSED(intf);
    PoFUsb* pof_usb = ctx;
    pof_cur = pof_usb;
    pof_usb->dev = dev;

    usbd_reg_config(dev, pof_usb_ep_config);
    usbd_reg_control(dev, pof_hid_control);
    UNUSED(pof_hid_control);
    usbd_connect(dev, true);

    pof_usb->thread = furi_thread_alloc();
    furi_thread_set_name(pof_usb->thread, "PoFUsb");
    furi_thread_set_stack_size(pof_usb->thread, 2 * 1024);
    furi_thread_set_context(pof_usb->thread, ctx);
    furi_thread_set_callback(pof_usb->thread, pof_thread_worker);

    furi_thread_start(pof_usb->thread);
}

static void pof_usb_deinit(usbd_device* dev) {
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);

    PoFUsb* pof_usb = pof_cur;
    if(!pof_usb || pof_usb->dev != dev) {
        return;
    }
    pof_cur = NULL;

    furi_assert(pof_usb->thread);
    furi_thread_flags_set(furi_thread_get_id(pof_usb->thread), EventExit);
    furi_thread_join(pof_usb->thread);
    furi_thread_free(pof_usb->thread);
    pof_usb->thread = NULL;

    free(pof_usb->usb.str_prod_descr);
    pof_usb->usb.str_prod_descr = NULL;
    free(pof_usb->usb.str_serial_descr);
    pof_usb->usb.str_serial_descr = NULL;
    free(pof_usb);
}

static void pof_usb_send(usbd_device* dev, uint8_t* buf, uint16_t len) {
    // Hide frequent responses
    if(buf[0] != 'S' && buf[0] != 'J') {
        FURI_LOG_RAW_D("> ");
        for(size_t i = 0; i < len; i++) {
            FURI_LOG_RAW_D("%02x", buf[i]);
        }
        FURI_LOG_RAW_D("\r\n");
    }
    usbd_ep_write(dev, POF_USB_EP_IN, buf, len);
}

static int32_t pof_usb_receive(usbd_device* dev, uint8_t* buf, uint16_t max_len) {
    int32_t len = usbd_ep_read(dev, POF_USB_EP_OUT, buf, max_len);
    return ((len < 0) ? 0 : len);
}

static void pof_usb_wakeup(usbd_device* dev) {
    UNUSED(dev);
}

static void pof_usb_suspend(usbd_device* dev) {
    PoFUsb* pof_usb = pof_cur;
    if(!pof_usb || pof_usb->dev != dev) return;
}

static void pof_usb_rx_ep_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    UNUSED(dev);
    UNUSED(event);
    UNUSED(ep);
    PoFUsb* pof_usb = pof_cur;
    furi_thread_flags_set(furi_thread_get_id(pof_usb->thread), EventRx);
}

static void pof_usb_tx_ep_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    UNUSED(dev);
    UNUSED(event);
    UNUSED(ep);
    PoFUsb* pof_usb = pof_cur;
    furi_thread_flags_set(furi_thread_get_id(pof_usb->thread), EventTxComplete);
}

static usbd_respond pof_usb_ep_config(usbd_device* dev, uint8_t cfg) {
    switch(cfg) {
    case 0: // deconfig
        usbd_ep_deconfig(dev, POF_USB_EP_OUT);
        usbd_ep_deconfig(dev, POF_USB_EP_IN);
        usbd_ep_deconfig(dev, POF_USB_X360_AUDIO_EP_IN1);
        usbd_ep_deconfig(dev, POF_USB_X360_AUDIO_EP_IN2);
        usbd_ep_deconfig(dev, POF_USB_X360_AUDIO_EP_OUT1);
        usbd_ep_deconfig(dev, POF_USB_X360_AUDIO_EP_OUT2);
        usbd_ep_deconfig(dev, POF_USB_X360_PLUGIN_MODULE_EP_IN);
        usbd_reg_endpoint(dev, POF_USB_EP_OUT, NULL);
        usbd_reg_endpoint(dev, POF_USB_EP_IN, NULL);
        usbd_reg_endpoint(dev, POF_USB_X360_AUDIO_EP_IN1, NULL);
        usbd_reg_endpoint(dev, POF_USB_X360_AUDIO_EP_IN2, NULL);
        usbd_reg_endpoint(dev, POF_USB_X360_AUDIO_EP_OUT1, NULL);
        usbd_reg_endpoint(dev, POF_USB_X360_AUDIO_EP_OUT2, NULL);
        usbd_reg_endpoint(dev, POF_USB_X360_PLUGIN_MODULE_EP_IN, NULL);
        return usbd_ack;
    case 1: // config
        usbd_ep_config(dev, POF_USB_EP_IN, USB_EPTYPE_INTERRUPT, POF_USB_EP_IN_SIZE);
        usbd_ep_config(dev, POF_USB_EP_OUT, USB_EPTYPE_INTERRUPT, POF_USB_EP_OUT_SIZE);
        usbd_ep_config(dev, POF_USB_X360_AUDIO_EP_IN1, USB_EPTYPE_INTERRUPT, POF_USB_EP_IN_SIZE);
        usbd_ep_config(dev, POF_USB_X360_AUDIO_EP_IN2, USB_EPTYPE_INTERRUPT, POF_USB_EP_OUT_SIZE);
        usbd_ep_config(dev, POF_USB_X360_AUDIO_EP_OUT1, USB_EPTYPE_INTERRUPT, POF_USB_EP_IN_SIZE);
        usbd_ep_config(dev, POF_USB_X360_AUDIO_EP_OUT2, USB_EPTYPE_INTERRUPT, POF_USB_EP_OUT_SIZE);
        usbd_ep_config(dev, POF_USB_X360_PLUGIN_MODULE_EP_IN, USB_EPTYPE_INTERRUPT, POF_USB_EP_OUT_SIZE);
        usbd_reg_endpoint(dev, POF_USB_EP_IN, pof_usb_tx_ep_callback);
        usbd_reg_endpoint(dev, POF_USB_EP_OUT, pof_usb_rx_ep_callback);
        return usbd_ack;
    }
    return usbd_fail;
}

struct PoFUsbDescriptor {
    struct usb_config_descriptor config;
    struct usb_interface_descriptor intf;
    struct usb_hid_descriptor hid_desc;
    struct usb_endpoint_descriptor ep_in;
    struct usb_endpoint_descriptor ep_out;
} __attribute__((packed));

struct usb_xbox_intf_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t reserved[2];
    uint8_t subtype;
    uint8_t reserved2;
    uint8_t bEndpointAddressIn;
    uint8_t bMaxDataSizeIn;
    uint8_t reserved3[5];
    uint8_t bEndpointAddressOut;
    uint8_t bMaxDataSizeOut;
    uint8_t reserved4[2];
} __attribute__((packed));

struct PoFUsbDescriptorXbox360 {
    struct usb_config_descriptor config;
    struct usb_interface_descriptor intf;
    struct usb_xbox_intf_descriptor xbox_desc;
    struct usb_endpoint_descriptor ep_in;
    struct usb_endpoint_descriptor ep_out;
    struct usb_interface_descriptor intfAudio;
    uint8_t audio_desc[0x1B];
    struct usb_endpoint_descriptor ep_in_audio1;
    struct usb_endpoint_descriptor ep_out_audio1;
    struct usb_endpoint_descriptor ep_in_audio2;
    struct usb_endpoint_descriptor ep_out_audio2;
    struct usb_interface_descriptor intfPluginModule;
    uint8_t plugin_module_desc[0x09];
    struct usb_endpoint_descriptor ep_in_plugin_module;
    struct usb_interface_descriptor intfSecurity;
    uint8_t security_desc[0x06];
} __attribute__((packed));
struct XInputVibrationCapabilities_t {
    uint8_t rid;
    uint8_t rsize;
    uint8_t padding;
    uint8_t left_motor;
    uint8_t right_motor;
    uint8_t padding_2[3];
} __attribute__((packed)) ;

struct XInputInputCapabilities_t {
    uint8_t rid;
    uint8_t rsize;
    uint16_t buttons;
    uint8_t leftTrigger;
    uint8_t rightTrigger;
    uint16_t leftThumbX;
    uint16_t leftThumbY;
    uint16_t rightThumbX;
    uint16_t rightThumbY;
    uint8_t reserved[4];
    uint16_t flags;
} __attribute__((packed));
static const struct usb_device_descriptor usb_pof_dev_descr = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = VERSION_BCD(2, 0, 0),
    .bDeviceClass = USB_CLASS_PER_INTERFACE,
    .bDeviceSubClass = USB_SUBCLASS_NONE,
    .bDeviceProtocol = USB_PROTO_NONE,
    .bMaxPacketSize0 = USB_EP0_SIZE,
    .idVendor = POF_USB_VID,
    .idProduct = POF_USB_PID,
    .bcdDevice = VERSION_BCD(1, 0, 0),
    .iManufacturer = 1, // UsbDevManuf
    .iProduct = 2, // UsbDevProduct
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

static const struct usb_device_descriptor usb_pof_dev_descr_xbox_360 = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = VERSION_BCD(2, 0, 0),
    .bDeviceClass = USB_CLASS_PER_INTERFACE,
    .bDeviceSubClass = USB_SUBCLASS_NONE,
    .bDeviceProtocol = USB_PROTO_NONE,
    .bMaxPacketSize0 = USB_EP0_SIZE,
    .idVendor = POF_USB_VID,
    .idProduct = POF_USB_PID_X360,
    .bcdDevice = VERSION_BCD(1, 0, 0),
    .iManufacturer = 1, // UsbDevManuf
    .iProduct = 2, // UsbDevProduct
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

static const uint8_t xbox_serial[] = {0x12, 0x14, 0x32, 0xEF};

static const struct XInputVibrationCapabilities_t XInputVibrationCapabilities = {
    rid : 0x00,
    rsize : sizeof(struct XInputVibrationCapabilities_t),
    padding : 0x00,
    left_motor : 0x00,
    right_motor : 0x00,
    padding_2 : {0x00, 0x00, 0x00}
};
static const struct XInputInputCapabilities_t XInputInputCapabilities = {
    rid : 0x00,
    rsize : sizeof(struct XInputInputCapabilities_t),
    buttons : 0x0000,
    leftTrigger : 0x00,
    rightTrigger : 0x00,
    leftThumbX : 0x0000,
    leftThumbY : 0x0000,
    rightThumbX : 0x0000,
    rightThumbY : 0x0000,
    reserved : {0x00, 0x00, 0x00, 0x00},
    flags : 0x00
};

static const struct PoFUsbDescriptor usb_pof_cfg_descr = {
    .config =
        {
            .bLength = sizeof(struct usb_config_descriptor),
            .bDescriptorType = USB_DTYPE_CONFIGURATION,
            .wTotalLength = sizeof(struct PoFUsbDescriptor),
            .bNumInterfaces = 1,
            .bConfigurationValue = 1,
            .iConfiguration = NO_DESCRIPTOR,
            .bmAttributes = USB_CFG_ATTR_RESERVED,
            .bMaxPower = USB_CFG_POWER_MA(500),
        },
    .intf =
        {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DTYPE_INTERFACE,
            .bInterfaceNumber = 0,
            .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = USB_CLASS_HID,
            .bInterfaceSubClass = USB_HID_SUBCLASS_NONBOOT,
            .bInterfaceProtocol = USB_HID_PROTO_NONBOOT,
            .iInterface = NO_DESCRIPTOR,
        },
    .hid_desc =
        {
            .bLength = sizeof(struct usb_hid_descriptor),
            .bDescriptorType = USB_DTYPE_HID,
            .bcdHID = VERSION_BCD(1, 1, 1),
            .bCountryCode = USB_HID_COUNTRY_NONE,
            .bNumDescriptors = 1,
            .bDescriptorType0 = USB_DTYPE_HID_REPORT,
            .wDescriptorLength0 = sizeof(hid_report_desc),
        },
    .ep_in =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = POF_USB_EP_IN,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = 0x40,
            .bInterval = HID_INTERVAL,
        },
    .ep_out =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = POF_USB_EP_OUT,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = 0x40,
            .bInterval = HID_INTERVAL,
        },
};


static const struct PoFUsbDescriptorXbox360 usb_pof_cfg_descr_x360 = {
    .config =
        {
            .bLength = sizeof(struct usb_config_descriptor),
            .bDescriptorType = USB_DTYPE_CONFIGURATION,
            .wTotalLength = sizeof(struct PoFUsbDescriptorXbox360),
            .bNumInterfaces = 4,
            .bConfigurationValue = 1,
            .iConfiguration = NO_DESCRIPTOR,
            .bmAttributes = USB_CFG_ATTR_RESERVED,
            .bMaxPower = USB_CFG_POWER_MA(500),
        },
    .intf =
        {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DTYPE_INTERFACE,
            .bInterfaceNumber = 0,
            .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = 0xFF,
            .bInterfaceSubClass = 0x5D,
            .bInterfaceProtocol = 0x01,
            .iInterface = NO_DESCRIPTOR,
        },
    .xbox_desc =
        {
            .bLength = sizeof(struct usb_xbox_intf_descriptor),
            .bDescriptorType = 0x21,
            .reserved = {0x10, 0x01},
            .subtype = 0x24,
            .reserved2 = 0x25,
            .bEndpointAddressIn = POF_USB_EP_IN,
            .bMaxDataSizeIn = 0x14,
            .reserved3 = {0x03, 0x03, 0x03, 0x04, 0x13},
            .bEndpointAddressOut = POF_USB_EP_OUT,
            .bMaxDataSizeOut = 0x08,
            .reserved4 = {0x03, 0x03},
        },
    .ep_in =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = POF_USB_EP_IN,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = 0x20,
            .bInterval = HID_INTERVAL,
        },
    .ep_out =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = POF_USB_EP_OUT,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = 0x40,
            .bInterval = HID_INTERVAL,
        },
    .intfAudio =
        {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DTYPE_INTERFACE,
            .bInterfaceNumber = 1,
            .bAlternateSetting = 0,
            .bNumEndpoints = 4,
            .bInterfaceClass = 0xFF,
            .bInterfaceSubClass = 0x5D,
            .bInterfaceProtocol = 0x03,
            .iInterface = NO_DESCRIPTOR,
        },
    .audio_desc =
        {0x1B, 0x21, 0x00, 0x01, 0x01, 0x01, POF_USB_X360_AUDIO_EP_IN1, 0x40, 0x01, POF_USB_X360_AUDIO_EP_OUT1,
            0x20, 0x16, POF_USB_X360_AUDIO_EP_IN2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16,
            POF_USB_X360_AUDIO_EP_OUT2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    .ep_in_audio1 =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = POF_USB_X360_AUDIO_EP_IN1,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = 0x20,
            .bInterval = 2,
        },
    .ep_out_audio1 =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = POF_USB_X360_AUDIO_EP_OUT1,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = 0x20,
            .bInterval = 4,
        },
    .ep_in_audio2 =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = POF_USB_X360_AUDIO_EP_IN2,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = 0x20,
            .bInterval = 0x40,
        },
    .ep_out_audio2 =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = POF_USB_X360_AUDIO_EP_OUT2,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = 0x20,
            .bInterval = 0x10,
        },

    .intfPluginModule =
        {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DTYPE_INTERFACE,
            .bInterfaceNumber = 2,
            .bAlternateSetting = 0,
            .bNumEndpoints = 1,
            .bInterfaceClass = 0xFF,
            .bInterfaceSubClass = 0x5D,
            .bInterfaceProtocol = 0x02,
            .iInterface = NO_DESCRIPTOR,
        },
    .plugin_module_desc =
        {0x09, 0x21, 0x00, 0x01, 0x01, 0x22, POF_USB_X360_PLUGIN_MODULE_EP_IN, 0x07, 0x00},
    .ep_in_plugin_module =
        {
            .bLength = sizeof(struct usb_endpoint_descriptor),
            .bDescriptorType = USB_DTYPE_ENDPOINT,
            .bEndpointAddress = POF_USB_X360_PLUGIN_MODULE_EP_IN,
            .bmAttributes = USB_EPTYPE_INTERRUPT,
            .wMaxPacketSize = 0x20,
            .bInterval = 16,
        },
    .intfSecurity =
        {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DTYPE_INTERFACE,
            .bInterfaceNumber = 3,
            .bAlternateSetting = 0,
            .bNumEndpoints = 0,
            .bInterfaceClass = 0xFF,
            .bInterfaceSubClass = 0xFD,
            .bInterfaceProtocol = 0x13,
            .iInterface = 4,
        },
    .security_desc =
        {0x06, 0x41, 0x00, 0x01, 0x01, 0x03},
};

/* Control requests handler */
static usbd_respond
    pof_hid_control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    UNUSED(callback);
    uint8_t wValueH = req->wValue >> 8;
    uint8_t wValueL = req->wValue & 0xFF;
    uint16_t length = req->wLength;

    PoFUsb* pof_usb = pof_cur;

    if (req->bmRequestType == 0xC0 && req->bRequest == USB_HID_GETREPORT && req->wValue == 0x0000) {
        dev->status.data_ptr = (uint8_t*)xbox_serial;
        dev->status.data_count = sizeof(xbox_serial);
        return usbd_ack;
    }
    if (req->bmRequestType == 0xC1 && req->bRequest == USB_HID_GETREPORT && req->wValue == 0x0100) {
        dev->status.data_ptr = (uint8_t*)&(XInputInputCapabilities);
        dev->status.data_count = sizeof(XInputInputCapabilities);
        return usbd_ack;
    }
    if (req->bmRequestType == 0xC1 && req->bRequest == USB_HID_GETREPORT && req->wValue == 0x0000) {
        dev->status.data_ptr = (uint8_t*)&(XInputVibrationCapabilities);
        dev->status.data_count = sizeof(XInputVibrationCapabilities);
        return usbd_ack;
    }

    /* HID control requests */
    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_INTERFACE | USB_REQ_CLASS) &&
       req->wIndex == 0) {
        switch(req->bRequest) {
        case USB_HID_SETIDLE:
            return usbd_ack;
        case USB_HID_SETPROTOCOL:
            return usbd_ack;
        case USB_HID_GETREPORT:
            dev->status.data_ptr = pof_usb->tx_data;
            dev->status.data_count = sizeof(pof_usb->tx_data);
            return usbd_ack;
        case USB_HID_SETREPORT:
            if(wValueH == HID_REPORT_TYPE_INPUT) {
                if(length == POF_USB_RX_MAX_SIZE) {
                    return usbd_ack;
                }
            } else if(wValueH == HID_REPORT_TYPE_OUTPUT) {
                memcpy(pof_usb->data, req->data, req->wLength);
                pof_usb->dataAvailable += req->wLength;
                furi_thread_flags_set(furi_thread_get_id(pof_usb->thread), EventRx);

                return usbd_ack;
            } else if(wValueH == HID_REPORT_TYPE_FEATURE) {
                return usbd_ack;
            }
            return usbd_fail;
        default:
            return usbd_fail;
        }
    }

    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_INTERFACE | USB_REQ_STANDARD) &&
       req->wIndex == 0 && req->bRequest == USB_STD_GET_DESCRIPTOR) {
        switch(wValueH) {
        case USB_DTYPE_HID:
            dev->status.data_ptr = (uint8_t*)&(usb_pof_cfg_descr.hid_desc);
            dev->status.data_count = sizeof(usb_pof_cfg_descr.hid_desc);
            return usbd_ack;
        case USB_DTYPE_HID_REPORT:
            dev->status.data_ptr = (uint8_t*)hid_report_desc;
            dev->status.data_count = sizeof(hid_report_desc);
            return usbd_ack;
        default:
            return usbd_fail;
        }
    }
    if(((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) ==
           (USB_REQ_DEVICE | USB_REQ_STANDARD) && req->bRequest == USB_STD_GET_DESCRIPTOR) {
        switch(wValueH) {
        case USB_DTYPE_STRING:
            if (wValueL == 4) {
                dev->status.data_ptr = (uint8_t*)&dev_security_desc;
                dev->status.data_count = dev_security_desc.bLength;
                return usbd_ack;
            }
            return usbd_fail;
        default:
            return usbd_fail;
        }
    }
    return usbd_fail;
}

PoFUsb* pof_usb_start(VirtualPortal* virtual_portal) {
    PoFUsb* pof_usb = malloc(sizeof(PoFUsb));
    pof_usb->virtual_portal = virtual_portal;
    pof_usb->dataAvailable = 0;

    pof_usb->usb_prev = furi_hal_usb_get_config();
    pof_usb->usb.init = pof_usb_init;
    pof_usb->usb.deinit = pof_usb_deinit;
    pof_usb->usb.wakeup = pof_usb_wakeup;
    pof_usb->usb.suspend = pof_usb_suspend;
    if (virtual_portal->type == PoFHID) {
        pof_usb->usb.dev_descr = (struct usb_device_descriptor*)&usb_pof_dev_descr;
        pof_usb->usb.cfg_descr = (void*)&usb_pof_cfg_descr;
    } else if (virtual_portal->type == PoFXbox360) {
        pof_usb->usb.dev_descr = (struct usb_device_descriptor*)&usb_pof_dev_descr_xbox_360;
        pof_usb->usb.cfg_descr = (void*)&usb_pof_cfg_descr_x360;
    }
    pof_usb->usb.str_manuf_descr = (void*)&dev_manuf_desc;
    pof_usb->usb.str_prod_descr = (void*)&dev_product_desc;
    pof_usb->usb.str_serial_descr = NULL;
    if(!furi_hal_usb_set_config(&pof_usb->usb, pof_usb)) {
        FURI_LOG_E(TAG, "USB locked, can not start");
        if(pof_usb->usb.str_manuf_descr) {
            free(pof_usb->usb.str_manuf_descr);
        }
        if(pof_usb->usb.str_prod_descr) {
            free(pof_usb->usb.str_prod_descr);
        }
        if(pof_usb->usb.str_serial_descr) {
            free(pof_usb->usb.str_serial_descr);
        }

        free(pof_usb);
        return NULL;
    }
    return pof_usb;
}

void pof_usb_stop(PoFUsb* pof_usb) {
    furi_hal_usb_set_config(pof_usb->usb_prev, NULL);
}
