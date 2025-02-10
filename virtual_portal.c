#include "virtual_portal.h"

#define TAG "VirtualPortal"

#define BLOCK_SIZE 16

static const NotificationSequence pof_sequence_cyan = {
    &message_blink_start_10,
    &message_blink_set_color_cyan,
    NULL,
};

VirtualPortal* virtual_portal_alloc(NotificationApp* notifications) {
    VirtualPortal* virtual_portal = malloc(sizeof(VirtualPortal));
    virtual_portal->notifications = notifications;

    for(int i = 0; i < POF_TOKEN_LIMIT; i++) {
        virtual_portal->tokens[i] = pof_token_alloc();
    }
    virtual_portal->sequence_number = 0;
    virtual_portal->active = false;

    return virtual_portal;
}

void virtual_portal_free(VirtualPortal* virtual_portal) {
    for(int i = 0; i < POF_TOKEN_LIMIT; i++) {
        pof_token_free(virtual_portal->tokens[i]);
        virtual_portal->tokens[i] = NULL;
    }

    free(virtual_portal);
}

uint8_t virtual_portal_next_sequence(VirtualPortal* virtual_portal) {
    if(virtual_portal->sequence_number == 0xff) {
        virtual_portal->sequence_number = 0;
    }
    return virtual_portal->sequence_number++;
}

int virtual_portal_activate(VirtualPortal* virtual_portal, uint8_t* message, uint8_t* response) {
    virtual_portal->active = (message[1] == 1);

    response[0] = message[0];
    response[1] = message[1];
    response[2] = 0xFF;
    response[3] = 0x77;
    return 4;
}

int virtual_portal_reset(VirtualPortal* virtual_portal, uint8_t* message, uint8_t* response) {
    UNUSED(message);
    virtual_portal->active = false;
    virtual_portal->sequence_number = 0;

    uint8_t index = 0;
    response[index++] = 'R';
    response[index++] = 0x02;
    response[index++] = 0x19;
    //response[index++] = 0x0a;
    //response[index++] = 0x03;
    //response[index++] = 0x02;
    // https://github.com/tresni/PoweredPortals/wiki/USB-Protocols
    // Wii Wireless: 01 29 00 00
    // Wii Wired: 02 0a 03 02 (Giants: works)
    // Arduboy: 02 19 (Trap team: works)
    return index;
}

int virtual_portal_status(VirtualPortal* virtual_portal, uint8_t* response) {
    response[0] = 'S';

    for(size_t i = 0; i < POF_TOKEN_LIMIT; i++) {
        // Can't use bit_lib since it uses the opposite endian
        if(virtual_portal->tokens[i]->loaded) {
            response[1 + i / 4] |= 1 << (i * 2 + 0);
        }
        if(virtual_portal->tokens[i]->change) {
            response[1 + i / 4] |= 1 << (i * 2 + 1);
        }

        virtual_portal->tokens[i]->change = false;
    }
    response[5] = virtual_portal_next_sequence(virtual_portal);
    response[6] = 1;

    return 7;
}

int virtual_portal_send_status(VirtualPortal* virtual_portal, uint8_t* response) {
    if(virtual_portal->active || virtual_portal->type == PoFXbox360) {
        notification_message(virtual_portal->notifications, &pof_sequence_cyan);
        return virtual_portal_status(virtual_portal, response);
    }
    return 0;
}

// 4d01ff0000d0077d6c2a77a400000000
int virtual_portal_m(VirtualPortal* virtual_portal, uint8_t* message, uint8_t* response) {
    UNUSED(virtual_portal);
    virtual_portal->speaker = (message[1] == 1);

    char display[33] = {0};
    for(size_t i = 0; i < BLOCK_SIZE; i++) {
        snprintf(display + (i * 2), sizeof(display), "%02x", message[i]);
    }
    FURI_LOG_I(TAG, "M %s", display);

    size_t index = 0;
    response[index++] = 'M';
    response[index++] = message[1];
    response[index++] = 0x00;
    response[index++] = 0x19;
    return index;
}

int virtual_portal_j(VirtualPortal* virtual_portal, uint8_t* message, uint8_t* response) {
    UNUSED(virtual_portal);

    char display[33] = {0};
    for(size_t i = 0; i < BLOCK_SIZE; i++) {
        snprintf(display + (i * 2), sizeof(display), "%02x", message[i]);
    }
    // FURI_LOG_I(TAG, "J %s", display);

    // https://marijnkneppers.dev/posts/reverse-engineering-skylanders-toys-to-life-mechanics/
    size_t index = 0;
    response[index++] = 'J';
    return index;
}

int virtual_portal_query(VirtualPortal* virtual_portal, uint8_t* message, uint8_t* response) {
    int index = message[1];
    int blockNum = message[2];
    int arrayIndex = index & 0x0f;
    FURI_LOG_I(TAG, "Query %d %d", arrayIndex, blockNum);

    PoFToken* pof_token = virtual_portal->tokens[arrayIndex];
    NfcDevice* nfc_device = pof_token->nfc_device;
    const MfClassicData* data = nfc_device_get_data(nfc_device, NfcProtocolMfClassic);
    const MfClassicBlock block = data->block[blockNum];

    response[0] = 'Q';
    response[1] = 0x20 | arrayIndex;
    response[2] = blockNum;
    memcpy(response + 3, block.data, BLOCK_SIZE);
    return 3 + BLOCK_SIZE;
}

int virtual_portal_write(VirtualPortal* virtual_portal, uint8_t* message, uint8_t* response) {
    int index = message[1];
    int blockNum = message[2];
    int arrayIndex = index & 0x0f;

    char display[33] = {0};
    for(size_t i = 0; i < BLOCK_SIZE; i++) {
        snprintf(display + (i * 2), sizeof(display), "%02x", message[3 + i]);
    }
    FURI_LOG_I(TAG, "Write %d %d %s", arrayIndex, blockNum, display);

    PoFToken* pof_token = virtual_portal->tokens[arrayIndex];
    NfcDevice* nfc_device = pof_token->nfc_device;

    MfClassicData* data = mf_classic_alloc();
    nfc_device_copy_data(nfc_device, NfcProtocolMfClassic, data);
    MfClassicBlock* block = &data->block[blockNum];

    memcpy(block->data, message + 3, BLOCK_SIZE);
    nfc_device_set_data(nfc_device, NfcProtocolMfClassic, data);

    mf_classic_free(data);

    response[0] = 'W';
    response[1] = index;
    response[2] = blockNum;
    return 3;
}

// 32 byte message, 32 byte response;
int virtual_portal_process_message(
    VirtualPortal* virtual_portal,
    uint8_t* message,
    uint8_t* response) {
    memset(response, 0, 32);
    switch(message[0]) {
    case 'A':
        FURI_LOG_D(TAG, "process %c", message[0]);
        return virtual_portal_activate(virtual_portal, message, response);
    case 'C': //Ring color R G B
        return 0;
    case 'J':
        // https://github.com/flyandi/flipper_zero_rgb_led
        return virtual_portal_j(virtual_portal, message, response);
    case 'L':
        return 0; //No response
    case 'M':
        return virtual_portal_m(virtual_portal, message, response);
    case 'Q': //Query
        FURI_LOG_D(TAG, "process %c", message[0]);
        return virtual_portal_query(virtual_portal, message, response);
    case 'R':
        FURI_LOG_D(TAG, "process %c", message[0]);
        return virtual_portal_reset(virtual_portal, message, response);
    case 'S': //Status
        FURI_LOG_D(TAG, "process %c", message[0]);
        return virtual_portal_status(virtual_portal, response);
    case 'V':
        return 0;
    case 'W': //Write
        FURI_LOG_D(TAG, "process %c", message[0]);
        return virtual_portal_write(virtual_portal, message, response);
    case 'Z':
        return 0;
    default:
        FURI_LOG_W(TAG, "Unhandled command %c", message[0]);
        return 0; //No response
    }

    return 0;
}
