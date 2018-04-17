// Copyright Microsoft Corporation
// Copyright Oberon microsystems, Inc
// Copyright GHI Electronics, LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string.h>
#include <TinyCLR.h>
#include <Device.h>

#define __min(a,b)  (((a) < (b)) ? (a) : (b))

#if defined(__GNUC__)
#define PACKED(x) x __attribute__((packed))
#elif defined(arm) || defined(__arm)
#define PACKED(x) __packed x
#endif

///////////////////////////////////////////////////////////////////////////////////////////
/// USB Debugger driver
///////////////////////////////////////////////////////////////////////////////////////////
// USB 2.0 host requests
#define USB_GET_STATUS           0
#define USB_CLEAR_FEATURE        1
#define USB_SET_FEATURE          3
#define USB_SET_ADDRESS          5
#define USB_GET_DESCRIPTOR       6
#define USB_SET_DESCRIPTOR       7
#define USB_GET_CONFIGURATION    8
#define USB_SET_CONFIGURATION    9

// USB 2.0 defined descriptor types
#define USB_DEVICE_DESCRIPTOR_TYPE        1
#define USB_CONFIGURATION_DESCRIPTOR_TYPE 2
#define USB_STRING_DESCRIPTOR_TYPE        3
#define USB_INTERFACE_DESCRIPTOR_TYPE     4
#define USB_ENDPOINT_DESCRIPTOR_TYPE      5

// USB 2.0 host request type defines
#define USB_SETUP_RECIPIENT(n)          ((n) & 0x0F)
#define USB_SETUP_RECIPIENT_DEVICE             0x00
#define USB_SETUP_RECIPIENT_INTERFACE          0x01
#define USB_SETUP_RECIPIENT_ENDPOINT           0x02

// Local device status defines
#define USB_STATUS_DEVICE_SELF_POWERED   0x0001
#define USB_STATUS_DEVICE_REMOTE_WAKEUP  0x0002

#define USB_STATUS_ENDPOINT_HALT         0x0001

#define USB_FEATURE_ENDPOINT_HALT        0x0000
#define USB_FEATURE_DEVICE_REMOTE_WAKEUP 0x0001

// Local device possible states
#define USB_DEVICE_STATE_DETACHED       0
#define USB_DEVICE_STATE_ATTACHED       1
#define USB_DEVICE_STATE_POWERED        2
#define USB_DEVICE_STATE_DEFAULT        3
#define USB_DEVICE_STATE_ADDRESS        4
#define USB_DEVICE_STATE_CONFIGURED     5
#define USB_DEVICE_STATE_SUSPENDED      6
#define USB_DEVICE_STATE_UNINITIALIZED  0xFF

// Possible responses to host requests
#define USB_STATE_DATA                  0
#define USB_STATE_STALL                 1
#define USB_STATE_DONE                  2
#define USB_STATE_ADDRESS               3
#define USB_STATE_STATUS                4
#define USB_STATE_CONFIGURATION         5
#define USB_STATE_REMOTE_WAKEUP         6


// ATTENTION:
// 2.0 is the lowest version that works with WinUSB on Windows 8!!!
// use older values below if you do not care about that

#define DEVICE_RELEASE_VERSION              0x0200

//string descriptor
#define USB_STRING_DESCRIPTOR_SIZE          32

// index for the strings
#define MANUFACTURER_NAME_INDEX             1
#define PRODUCT_NAME_INDEX                  2
#define SERIAL_NUMBER_INDEX                 0

// configuration for extended descriptor
#define OS_DESCRIPTOR_EX_VERSION            0x0100

#define USB_DISPLAY_STRING_NUM     4
#define USB_FRIENDLY_STRING_NUM    5

#define OS_DESCRIPTOR_STRING_INDEX        0xEE
#define OS_DESCRIPTOR_STRING_VENDOR_CODE  0xA5

// USB 2.0 response structure lengths
#define USB_DEVICE_DESCRIPTOR_LENGTH             18
#define USB_CONFIGURATION_DESCRIPTOR_LENGTH       9
#define USB_STRING_DESCRIPTOR_HEADER_LENGTH       2


// USB configuration list structures
#define USB_END_DESCRIPTOR_MARKER           0x00
#define USB_DEVICE_DESCRIPTOR_MARKER        0x01
#define USB_CONFIGURATION_DESCRIPTOR_MARKER 0x02
#define USB_STRING_DESCRIPTOR_MARKER        0x03
#define USB_GENERIC_DESCRIPTOR_MARKER       0xFF

// configuration Descriptor
#define USB_ATTRIBUTE_REMOTE_WAKEUP    0x20
#define USB_ATTRIBUTE_SELF_POWER       0x40
#define USB_ATTRIBUTE_BASE             0x80

// Endpoint Direction
#define USB_ENDPOINT_DIRECTION_IN 0x80
#define USB_ENDPOINT_DIRECTION_OUT 0x00
#define USB_ENDPOINT_NULL 0xFF

// Endpoint Attribute
#define ENDPOINT_INUSED_MASK        0x01
#define ENDPOINT_DIR_IN_MASK        0x02
#define ENDPOINT_DIR_OUT_MASK       0x04

#define USB_ENDPOINT_ATTRIBUTE_BULK 2
#define USB_MAX_DATA_PACKET_SIZE 64

// This version of the USB code supports only one language - which
// is not specified by USB configuration records - it is defined here.
// This is the String 0 descriptor.This array includes the String descriptor
// header and exactly one language.

#define USB_LANGUAGE_DESCRIPTOR_SIZE 4

// USB 2.0 request packet from host
PACKED(struct) USB_SETUP_PACKET {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

PACKED(struct) USB_DYNAMIC_CONFIGURATION;

struct USB_PACKET64 {
    uint32_t Size;
    uint8_t  Buffer[USB_MAX_DATA_PACKET_SIZE];
};

struct USB_PIPE_MAP {
    uint8_t RxEP;
    uint8_t TxEP;
};

struct USB_CONTROLLER_STATE;

typedef void(*USB_NEXT_CALLBACK)(USB_CONTROLLER_STATE*);

struct USB_CONTROLLER_STATE {
    bool                                                        initialized;
    uint8_t                                                     currentState;
    uint8_t                                                     controllerNum;
    uint32_t                                                    event;

    const USB_DYNAMIC_CONFIGURATION*                            configuration;

    /* queues & maxPacketSize must be initialized by the HAL */
    USB_PACKET64                                   	            *queues[CONCAT(DEVICE_TARGET, _USB_QUEUE_SIZE)];
    uint8_t                                                     currentPacketOffset[CONCAT(DEVICE_TARGET, _USB_QUEUE_SIZE)];
    uint8_t                                                     maxPacketSize[CONCAT(DEVICE_TARGET, _USB_QUEUE_SIZE)];
    bool                                                        isTxQueue[CONCAT(DEVICE_TARGET, _USB_QUEUE_SIZE)];

    /* Arbitrarily as many pipes as endpoints since that is the maximum number of pipes
       necessary to represent the maximum number of endpoints */
    USB_PIPE_MAP                                                pipes[CONCAT(DEVICE_TARGET, _USB_QUEUE_SIZE)];

    /* used for transferring packets between upper & lower */
    uint8_t*                                                    ptrData;
    uint8_t                                                     dataSize;

    /* USB hardware information */
    uint8_t                                                     address;
    uint8_t                                                     deviceState;
    uint8_t                                                     packetSize;
    uint8_t                                                     configurationNum;
    uint32_t                                                    firstGetDescriptor;

    /* USB status information, used in
       GET_STATUS, SET_FEATURE, CLEAR_FEATURE */
    uint16_t                                                    deviceStatus;

    uint16_t                                                    endpointType;
    uint16_t*                                                   endpointStatus;
    uint8_t                                                     endpointCount;
    uint8_t                                                     endpointStatusChange;

    /* callback function for getting next packet */
    USB_NEXT_CALLBACK                                           dataCallback;

    /* for helping out upper layer during callbacks */
    uint8_t*                                                    residualData;
    uint16_t                                                    residualCount;
    uint16_t                                                    expected;
};

PACKED(struct) TinyCLR_UsbClient_DescriptorHeader {
    uint8_t  marker;
    uint8_t  iValue;
    uint16_t size;
};

PACKED(struct) TinyCLR_UsbClient_GenericDescriptorHeader {
    TinyCLR_UsbClient_DescriptorHeader header;

    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
};

PACKED(struct) TinyCLR_UsbClient_DeviceDescriptor {
    TinyCLR_UsbClient_DescriptorHeader header;

    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

PACKED(struct) TinyCLR_UsbClient_InterfaceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
};

PACKED(struct) TinyCLR_UsbClient_EndpointDescriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
};

PACKED(struct) TinyCLR_UsbClient_ClassDescriptorHeader {
    uint8_t bLength;
    uint8_t bDescriptorType;
};

PACKED(struct) TinyCLR_UsbClient_StringDescriptorHeader {
    TinyCLR_UsbClient_DescriptorHeader header;

    uint8_t bLength;
    uint8_t bDescriptorType;
    wchar_t stringDescriptor[32];
};

PACKED(struct) TinyCLR_UsbClient_ConfigurationDescriptor {
    TinyCLR_UsbClient_DescriptorHeader header;

    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;

    TinyCLR_UsbClient_InterfaceDescriptor   itfc0;
    TinyCLR_UsbClient_EndpointDescriptor    epWrite;
    TinyCLR_UsbClient_EndpointDescriptor    epRead;
};

PACKED(struct) TinyCLR_UsbClient_OsStringDescriptor {
    TinyCLR_UsbClient_DescriptorHeader header;

    uint8_t   bLength;
    uint8_t   bDescriptorType;
    wchar_t signature[7];
    uint8_t   bMS_VendorCode;
    uint8_t   padding;
};

PACKED(struct) TinyCLR_UsbClient_XCompatibleOsId {
    TinyCLR_UsbClient_GenericDescriptorHeader header;

    uint32_t dwLength;
    uint16_t bcdVersion;
    uint16_t wIndex;
    uint8_t  bCount;
    uint8_t  padding1[7];
    uint8_t  bFirstInterfaceNumber;
    uint8_t  reserved;
    uint8_t  compatibleID[8];
    uint8_t  subCompatibleID[8];
    uint8_t  padding2[6];
};

PACKED(struct) TinyCLR_UsbClient_XPropertiesOsWinUsb {
    TinyCLR_UsbClient_GenericDescriptorHeader header;

    uint32_t dwLength;
    uint16_t bcdVersion;
    uint16_t wIndex;
    uint16_t  bCount;

    uint32_t dwSize;
    uint32_t dwPropertyDataType;
    uint16_t wPropertyNameLengh;
    uint8_t  bPropertyName[40];
    uint32_t dwPropertyDataLengh;
    uint8_t  bPropertyData[78];
};

PACKED(struct) USB_DYNAMIC_CONFIGURATION {
    TinyCLR_UsbClient_DeviceDescriptor                  *device;
    TinyCLR_UsbClient_ConfigurationDescriptor           *config;
    TinyCLR_UsbClient_StringDescriptorHeader            *manHeader;
    TinyCLR_UsbClient_StringDescriptorHeader            *prodHeader;
    TinyCLR_UsbClient_StringDescriptorHeader            *displayStringHeader;
    TinyCLR_UsbClient_StringDescriptorHeader            *friendlyStringHeader;
    TinyCLR_UsbClient_OsStringDescriptor                *OS_String;
    TinyCLR_UsbClient_XCompatibleOsId                   *OS_XCompatible_ID;
    TinyCLR_UsbClient_XPropertiesOsWinUsb               *OS_XProperty;
    TinyCLR_UsbClient_DescriptorHeader                  *endList;
};

const TinyCLR_Api_Info* UsbClient_GetApi();
TinyCLR_Result UsbClient_Acquire(const TinyCLR_UsbClient_Provider* self);
TinyCLR_Result UsbClient_Release(const TinyCLR_UsbClient_Provider* self);
TinyCLR_Result UsbClient_Open(const TinyCLR_UsbClient_Provider* self, int32_t& pipe, TinyCLR_UsbClient_PipeMode mode);
TinyCLR_Result UsbClient_Close(const TinyCLR_UsbClient_Provider* self, int32_t pipe);
TinyCLR_Result UsbClient_Write(const TinyCLR_UsbClient_Provider* self, int32_t pipe, const uint8_t* data, size_t& length);
TinyCLR_Result UsbClient_Read(const TinyCLR_UsbClient_Provider* self, int32_t pipe, uint8_t* data, size_t& length);
TinyCLR_Result UsbClient_Flush(const TinyCLR_UsbClient_Provider* self, int32_t pipe);
TinyCLR_Result UsbClient_SetDeviceDescriptor(const TinyCLR_UsbClient_Provider* self, const void* descriptor, int32_t length);
TinyCLR_Result UsbClient_SetConfigDescriptor(const TinyCLR_UsbClient_Provider* self, const void* descriptor, int32_t length);
TinyCLR_Result UsbClient_SetStringDescriptor(const TinyCLR_UsbClient_Provider* self, TinyCLR_UsbClient_StringDescriptorType type, const wchar_t* value);
TinyCLR_Result UsbClient_SetDataReceivedHandler(const TinyCLR_UsbClient_Provider* self, TinyCLR_UsbClient_DataReceivedHandler handler);
TinyCLR_Result UsbClient_SetOsExtendedPropertyHandler(const TinyCLR_UsbClient_Provider* self, TinyCLR_UsbClient_OsExtendedPropertyHandler handler);

const TinyCLR_UsbClient_DescriptorHeader * UsbClient_FindRecord(USB_CONTROLLER_STATE* usbState, uint8_t marker, USB_SETUP_PACKET * iValue);

void UsbClient_ClearEvent(USB_CONTROLLER_STATE *usbState, uint32_t event);
void UsbClient_StateCallback(USB_CONTROLLER_STATE* usbState);
void UsbClient_ClearEndpoints(int32_t endpoint);

USB_PACKET64* UsbClient_RxEnqueue(USB_CONTROLLER_STATE* usbState, int32_t endpoint, bool& disableRx);
USB_PACKET64* UsbClient_TxDequeue(USB_CONTROLLER_STATE* usbState, int32_t endpoint);

uint8_t UsbClient_ControlCallback(USB_CONTROLLER_STATE* usbState);
int32_t UsbClient_GetBufferCount(int32_t endpoint);

bool CONCAT(DEVICE_TARGET, _UsbClient_Initialize(USB_CONTROLLER_STATE* usbState));
bool CONCAT(DEVICE_TARGET, _UsbClient_Uninitialize(USB_CONTROLLER_STATE* usbState));
bool CONCAT(DEVICE_TARGET, _UsbClient_StartOutput(USB_CONTROLLER_STATE* usbState, int32_t endpoint));
bool CONCAT(DEVICE_TARGET, _UsbClient_RxEnable(USB_CONTROLLER_STATE* usbState, int32_t endpoint));
