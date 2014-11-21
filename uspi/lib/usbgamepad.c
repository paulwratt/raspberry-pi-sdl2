//
// usbgamepad.c
//
// USPi - An USB driver for Raspberry Pi written in C
// Copyright (C) 2014  R. Stange <rsta2@o2online.de>
// Copyright (C) 2014  M. Maccaferri <macca@maccasoft.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <uspi/usbgamepad.h>
#include <uspi/usbhostcontroller.h>
#include <uspi/devicenameservice.h>
#include <uspi/assert.h>
#include <uspi/util.h>
#include <uspios.h>

// HID Report Items from HID 1.11 Section 6.2.2
#define HID_USAGE_PAGE      0x04
#define HID_USAGE           0x08
#define HID_COLLECTION      0xA0
#define HID_END_COLLECTION  0xC0
#define HID_REPORT_COUNT    0x94
#define HID_REPORT_SIZE     0x74
#define HID_USAGE_MIN       0x18
#define HID_USAGE_MAX       0x28
#define HID_LOGICAL_MIN     0x14
#define HID_LOGICAL_MAX     0x24
#define HID_PHYSICAL_MIN    0x34
#define HID_PHYSICAL_MAX    0x44
#define HID_INPUT           0x80
#define HID_REPORT_ID       0x84
#define HID_OUTPUT          0x90

// HID Report Usage Pages from HID Usage Tables 1.12 Section 3, Table 1
#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_KEY_CODES       0x07
#define HID_USAGE_PAGE_LEDS            0x08
#define HID_USAGE_PAGE_BUTTONS         0x09

// HID Report Usages from HID Usage Tables 1.12 Section 4, Table 6
#define HID_USAGE_POINTER  0x01
#define HID_USAGE_MOUSE    0x02
#define HID_USAGE_JOYSTICK 0x04
#define HID_USAGE_GAMEPAD  0x05
#define HID_USAGE_KEYBOARD 0x06
#define HID_USAGE_X        0x30
#define HID_USAGE_Y        0x31
#define HID_USAGE_Z        0x32
#define HID_USAGE_RX       0x33
#define HID_USAGE_RY       0x34
#define HID_USAGE_RZ       0x35
#define HID_USAGE_WHEEL    0x38

// HID Report Collection Types from HID 1.12 6.2.2.6
#define HID_COLLECTION_PHYSICAL    0
#define HID_COLLECTION_APPLICATION 1

// HID Input/Output/Feature Item Data (attributes) from HID 1.11 6.2.2.5
#define HID_ITEM_CONSTANT 0x1
#define HID_ITEM_VARIABLE 0x2
#define HID_ITEM_RELATIVE 0x4

static unsigned s_nDeviceNumber = 1;

static const char FromUSBPad[] = "usbpad";

static boolean USBGamePadDeviceStartRequest (TUSBGamePadDevice *pThis);
static void USBGamePadDeviceCompletionRoutine (TUSBRequest *pURB, void *pParam, void *pContext);

void USBGamePadDevice (TUSBGamePadDevice *pThis, TUSBDevice *pDevice)
{
	assert (pThis != 0);

	USBDeviceCopy (&pThis->m_USBDevice, pDevice);
	pThis->m_USBDevice.Configure = USBGamePadDeviceConfigure;

	pThis->m_pEndpointIn = 0;
    pThis->m_pEndpointOut = 0;
    pThis->m_pStatusHandler = 0;
	pThis->m_pURB = 0;
	pThis->m_pReportBuffer = 0;
	pThis->m_pHIDReportDescriptor = 0;
	pThis->m_usReportDescriptorLength = 0;

	pThis->m_State.idVendor = pDevice->m_pDeviceDesc->idVendor;
    pThis->m_State.idProduct = pDevice->m_pDeviceDesc->idProduct;
    pThis->m_State.idVersion = pDevice->m_pDeviceDesc->bcdDevice;

    pThis->m_State.x = pThis->m_State.y = pThis->m_State.z = 0;
    pThis->m_State.rx = pThis->m_State.ry = pThis->m_State.rz = 0;
    pThis->m_State.maximum = pThis->m_State.minimum = 0;

    pThis->m_State.nbuttons = 0;
    pThis->m_State.buttons = 0;

	pThis->m_pReportBuffer = malloc (64);
	assert (pThis->m_pReportBuffer != 0);
}

void _CUSBGamePadDevice (TUSBGamePadDevice *pThis)
{
	assert (pThis != 0);

    if (pThis->m_pHIDReportDescriptor != 0)
    {
        free (pThis->m_pHIDReportDescriptor);
        pThis->m_pHIDReportDescriptor = 0;
    }

	if (pThis->m_pReportBuffer != 0)
	{
		free (pThis->m_pReportBuffer);
		pThis->m_pReportBuffer = 0;
	}

	if (pThis->m_pEndpointIn != 0)
	{
		_USBEndpoint (pThis->m_pEndpointIn);
		free (pThis->m_pEndpointIn);
		pThis->m_pEndpointIn = 0;
	}

    if (pThis->m_pEndpointOut != 0)
    {
        _USBEndpoint (pThis->m_pEndpointOut);
        free (pThis->m_pEndpointOut);
        pThis->m_pEndpointOut = 0;
    }

	_USBDevice (&pThis->m_USBDevice);
}

static u32 BitGetUnsigned(void *buffer, u32 offset, u32 length)
{
    u8* bitBuffer;
    u8 mask;
    u32 result;

    bitBuffer = buffer;
    result = 0;
    for (u32 i = offset / 8, j = 0; i < (offset + length + 7) / 8; i++) {
        if (offset / 8 == (offset + length - 1) / 8) {
            mask = (1 << ((offset % 8) + length)) - (1 << (offset % 8));
            result = (bitBuffer[i] & mask) >> (offset % 8);
        } else if (i == offset / 8) {
            mask = 0x100 - (1 << (offset % 8));
            j += 8 - (offset % 8);
            result = ((bitBuffer[i] & mask) >> (offset % 8)) << (length - j);
        } else if (i == (offset + length - 1) / 8) {
            mask = (1 << ((offset % 8) + length)) - 1;
            result |= bitBuffer[i] & mask;
        } else {
            j += 8;
            result |= bitBuffer[i] << (length - j);
        }
    }

    return result;
}

static s32 BitGetSigned(void* buffer, u32 offset, u32 length) {
    u32 result = BitGetUnsigned(buffer, offset, length);

    if (result & (1 << (length - 1)))
        result |= 0xffffffff - ((1 << length) - 1);

    return result;
}

enum {
    None = 0,
    GamePad,
    GamePadButton,
    GamePadAxis,
};

static void USBGamePadDeviceDecodeReport(TUSBGamePadDevice *pThis)
{
    s32 item, arg;
    u32 offset = 0, size = 0, count = 0;
    s32 index = 0, map[MAX_AXIS] = { -1, -1, -1, -1, -1, -1 }, max = 0, min = 0;
    u32 state = None;

    u8 *pReportBuffer = pThis->m_pReportBuffer;
    s8 *pHIDReportDescriptor = (s8 *)pThis->m_pHIDReportDescriptor;
    u16 wReportDescriptorLength = pThis->m_usReportDescriptorLength;

    while (wReportDescriptorLength > 0) {
        item = *pHIDReportDescriptor++;
        wReportDescriptorLength--;

        switch(item & 0x03) {
            case 0:
                arg = 0;
                break;
            case 1:
                arg = *pHIDReportDescriptor++;
                wReportDescriptorLength--;
                break;
            case 2:
                arg = *pHIDReportDescriptor++ & 0xFF;
                arg = arg | (*pHIDReportDescriptor++ << 8);
                wReportDescriptorLength -= 2;
                break;
            default:
                arg = *pHIDReportDescriptor++;
                arg = arg | (*pHIDReportDescriptor++ << 8);
                arg = arg | (*pHIDReportDescriptor++ << 16);
                arg = arg | (*pHIDReportDescriptor++ << 24);
                wReportDescriptorLength -= 4;
                break;
        }

        if ((item & 0xFC) == HID_REPORT_ID) {
            if (BitGetUnsigned(pReportBuffer, 0, 8) != arg)
                break;
            offset += 8;
        }

        switch(item & 0xFC) {
            case HID_USAGE_PAGE:
                switch(arg) {
                    case HID_USAGE_PAGE_BUTTONS:
                        if (state == GamePad)
                            state = GamePadButton;
                        break;
                }
                break;
            case HID_USAGE:
                switch(arg) {
                    case HID_USAGE_JOYSTICK:
                    case HID_USAGE_GAMEPAD:
                        state = GamePad;
                        break;
                    case HID_USAGE_X:
                    case HID_USAGE_Y:
                    case HID_USAGE_Z:
                    case HID_USAGE_RX:
                    case HID_USAGE_RY:
                    case HID_USAGE_RZ:
                        map[index++] = arg;
                        if (state == GamePad)
                            state = GamePadAxis;
                        break;
                }
                break;
            case HID_LOGICAL_MIN:
            case HID_PHYSICAL_MIN:
                min = arg;
                break;
            case HID_LOGICAL_MAX:
            case HID_PHYSICAL_MAX:
                max = arg;
                break;
            case HID_REPORT_SIZE: // REPORT_SIZE
                size = arg;
                break;
            case HID_REPORT_COUNT: // REPORT_COUNT
                count = arg;
                break;
            case HID_INPUT:
                switch(arg) {
                    case 0x02: // INPUT(Data,Var,Abs)
                        if (state == GamePadAxis) {
                            pThis->m_State.minimum = min;
                            pThis->m_State.maximum = max;

                            for (int i = 0; i < count && i < MAX_AXIS; i++) {
                                int value = (min < 0) ? BitGetSigned(pReportBuffer, offset + i * size, size) :
                                                        BitGetUnsigned(pReportBuffer, offset + i * size, size);

                                switch(map[i]) {
                                    case HID_USAGE_X:
                                        pThis->m_State.x = value;
                                        pThis->m_State.flags |= FLAG_X;
                                        break;
                                    case HID_USAGE_Y:
                                        pThis->m_State.y = value;
                                        pThis->m_State.flags |= FLAG_Y;
                                        break;
                                    case HID_USAGE_Z:
                                        pThis->m_State.z = value;
                                        pThis->m_State.flags |= FLAG_Z;
                                        break;
                                    case HID_USAGE_RX:
                                        pThis->m_State.rx = value;
                                        pThis->m_State.flags |= FLAG_RX;
                                        break;
                                    case HID_USAGE_RY:
                                        pThis->m_State.ry = value;
                                        pThis->m_State.flags |= FLAG_RY;
                                        break;
                                    case HID_USAGE_RZ:
                                        pThis->m_State.rz = value;
                                        pThis->m_State.flags |= FLAG_RZ;
                                        break;
                                }
                            }

                            state = GamePad;
                        }
                        else if (state == GamePadButton) {
                            pThis->m_State.nbuttons = count;
                            pThis->m_State.buttons = BitGetUnsigned(pReportBuffer, offset, size * count);
                            state = GamePad;
                        }
                        break;
                }
                offset += count * size;
                break;
            case HID_OUTPUT:
                break;
        }
    }
}

boolean USBGamePadDeviceConfigure (TUSBDevice *pUSBDevice)
{
	TUSBGamePadDevice *pThis = (TUSBGamePadDevice *) pUSBDevice;
	assert (pThis != 0);

	TUSBConfigurationDescriptor *pConfDesc =
		(TUSBConfigurationDescriptor *) USBDeviceGetDescriptor (&pThis->m_USBDevice, DESCRIPTOR_CONFIGURATION);
	if (   pConfDesc == 0
	    || pConfDesc->bNumInterfaces <  1)
	{
		USBDeviceConfigurationError (&pThis->m_USBDevice, FromUSBPad);

		return FALSE;
	}

    TUSBInterfaceDescriptor *pInterfaceDesc =
        (TUSBInterfaceDescriptor *) USBDeviceGetDescriptor (&pThis->m_USBDevice, DESCRIPTOR_INTERFACE);
    if (   pInterfaceDesc == 0
        || pInterfaceDesc->bNumEndpoints      <  1
        || pInterfaceDesc->bInterfaceClass    != 0x03   // HID Class
        || pInterfaceDesc->bInterfaceSubClass != 0x00   // Boot Interface Subclass
        || pInterfaceDesc->bInterfaceProtocol != 0x00)  // GamePad
    {
        USBDeviceConfigurationError (&pThis->m_USBDevice, FromUSBPad);

        return FALSE;
    }

    pThis->m_ucInterfaceNumber  = pInterfaceDesc->bInterfaceNumber;
    pThis->m_ucAlternateSetting = pInterfaceDesc->bAlternateSetting;

    TUSBHIDDescriptor *pHIDDesc = (TUSBHIDDescriptor *) USBDeviceGetDescriptor (&pThis->m_USBDevice, DESCRIPTOR_HID);
    if (   pHIDDesc == 0
        || pHIDDesc->wReportDescriptorLength == 0)
    {
        USBDeviceConfigurationError (&pThis->m_USBDevice, FromUSBPad);

        return FALSE;
    }

    const TUSBEndpointDescriptor *pEndpointDesc;
    while ((pEndpointDesc = (TUSBEndpointDescriptor *) USBDeviceGetDescriptor (&pThis->m_USBDevice, DESCRIPTOR_ENDPOINT)) != 0)
    {
        if ((pEndpointDesc->bmAttributes & 0x3F) == 0x03)       // Interrupt
        {
            if ((pEndpointDesc->bEndpointAddress & 0x80) == 0x80)   // Input
            {
                if (pThis->m_pEndpointIn != 0)
                {
                    USBDeviceConfigurationError (&pThis->m_USBDevice, FromUSBPad);

                    return FALSE;
                }

                pThis->m_pEndpointIn = (TUSBEndpoint *) malloc (sizeof (TUSBEndpoint));
                assert (pThis->m_pEndpointIn != 0);
                USBEndpoint2 (pThis->m_pEndpointIn, &pThis->m_USBDevice, pEndpointDesc);
            }
            else                            // Output
            {
                if (pThis->m_pEndpointOut != 0)
                {
                    USBDeviceConfigurationError (&pThis->m_USBDevice, FromUSBPad);

                    return FALSE;
                }

                pThis->m_pEndpointOut = (TUSBEndpoint *) malloc (sizeof (TUSBEndpoint));
                assert (pThis->m_pEndpointOut != 0);
                USBEndpoint2 (pThis->m_pEndpointOut, &pThis->m_USBDevice, pEndpointDesc);
            }
        }
    }

	if (pThis->m_pEndpointIn == 0)
	{
		USBDeviceConfigurationError (&pThis->m_USBDevice, FromUSBPad);

		return FALSE;
	}

    pThis->m_usReportDescriptorLength = pHIDDesc->wReportDescriptorLength;
    pThis->m_pHIDReportDescriptor = (unsigned char *) malloc(pHIDDesc->wReportDescriptorLength);
    assert (pThis->m_pHIDReportDescriptor != 0);

    if (DWHCIDeviceGetDescriptor (USBDeviceGetHost (&pThis->m_USBDevice),
                    USBDeviceGetEndpoint0 (&pThis->m_USBDevice),
                    pHIDDesc->bReportDescriptorType, DESCRIPTOR_INDEX_DEFAULT,
                    pThis->m_pHIDReportDescriptor, pHIDDesc->wReportDescriptorLength, REQUEST_IN)
        != pHIDDesc->wReportDescriptorLength)
    {
        LogWrite (FromUSBPad, LOG_ERROR, "Cannot get HID report descriptor");

        return FALSE;
    }
    //DebugHexdump (pThis->m_pHIDReportDescriptor, pHIDDesc->wReportDescriptorLength, "hid");

    if (!USBDeviceConfigure (&pThis->m_USBDevice))
    {
        LogWrite (FromUSBPad, LOG_ERROR, "Cannot set configuration");

        return FALSE;
    }

	TString DeviceName;
	String (&DeviceName);
	StringFormat (&DeviceName, "upad%u", s_nDeviceNumber++);
	DeviceNameServiceAddDevice (DeviceNameServiceGet (), StringGet (&DeviceName), pThis, FALSE);

	_String (&DeviceName);

	return USBGamePadDeviceStartRequest (pThis);
}

void USBGamePadDeviceRegisterStatusHandler (TUSBGamePadDevice *pThis, TGamePadStatusHandler *pStatusHandler)
{
    assert (pThis != 0);
    assert (pPadStatusHandler != 0);
    pThis->m_pStatusHandler = pStatusHandler;
}

boolean USBGamePadDeviceStartRequest (TUSBGamePadDevice *pThis)
{
	assert (pThis != 0);

	assert (pThis->m_pEndpointIn != 0);
	assert (pThis->m_pReportBuffer != 0);

	assert (pThis->m_pURB == 0);
	pThis->m_pURB = malloc (sizeof (TUSBRequest));
	assert (pThis->m_pURB != 0);
	USBRequest (pThis->m_pURB, pThis->m_pEndpointIn, pThis->m_pReportBuffer, 8, 0);
	USBRequestSetCompletionRoutine (pThis->m_pURB, USBGamePadDeviceCompletionRoutine, 0, pThis);

	return DWHCIDeviceSubmitAsyncRequest (USBDeviceGetHost (&pThis->m_USBDevice), pThis->m_pURB);
}

void USBGamePadDeviceCompletionRoutine (TUSBRequest *pURB, void *pParam, void *pContext)
{
	TUSBGamePadDevice *pThis = (TUSBGamePadDevice *) pContext;
	assert (pThis != 0);

	assert (pURB != 0);
	assert (pThis->m_pURB == pURB);

	if (   USBRequestGetStatus (pURB) != 0
	    && USBRequestGetResultLength (pURB) > 0)
	{
        //DebugHexdump (pThis->m_pReportBuffer, 16, "report");
        if (pThis->m_pHIDReportDescriptor != 0 && pThis->m_pStatusHandler != 0)
        {
            USBGamePadDeviceDecodeReport (pThis);
            (*pThis->m_pStatusHandler) (&pThis->m_State);
        }
	}

	_USBRequest (pThis->m_pURB);
	free (pThis->m_pURB);
	pThis->m_pURB = 0;

	USBGamePadDeviceStartRequest (pThis);
}

void USBGamePadDeviceGetReport (TUSBGamePadDevice *pThis)
{
    s32 item, arg;
    s8 *pHIDReportDescriptor = (s8 *)pThis->m_pHIDReportDescriptor;
    u16 wReportDescriptorLength = pThis->m_usReportDescriptorLength;

    while (wReportDescriptorLength > 0) {
        item = *pHIDReportDescriptor++;
        wReportDescriptorLength--;

        switch(item & 0x03) {
            case 0:
                arg = 0;
                break;
            case 1:
                arg = *pHIDReportDescriptor++;
                wReportDescriptorLength--;
                break;
            case 2:
                arg = *pHIDReportDescriptor++ & 0xFF;
                arg = arg | (*pHIDReportDescriptor++ << 8);
                wReportDescriptorLength -= 2;
                break;
            default:
                arg = *pHIDReportDescriptor++;
                arg = arg | (*pHIDReportDescriptor++ << 8);
                arg = arg | (*pHIDReportDescriptor++ << 16);
                arg = arg | (*pHIDReportDescriptor++ << 24);
                wReportDescriptorLength -= 4;
                break;
        }

        if ((item & 0xFC) == HID_REPORT_ID) {
            if (DWHCIDeviceControlMessage (USBDeviceGetHost (&pThis->m_USBDevice),
                               USBDeviceGetEndpoint0 (&pThis->m_USBDevice),
                               REQUEST_IN | REQUEST_CLASS | REQUEST_TO_INTERFACE,
                               GET_REPORT, arg << 8,
                               pThis->m_ucInterfaceNumber,
                               pThis->m_pReportBuffer, 8) > 0)
            {
                USBGamePadDeviceDecodeReport (pThis);
                return;
            }
        }
    }

    if (DWHCIDeviceControlMessage (USBDeviceGetHost (&pThis->m_USBDevice),
                       USBDeviceGetEndpoint0 (&pThis->m_USBDevice),
                       REQUEST_IN | REQUEST_CLASS | REQUEST_TO_INTERFACE,
                       GET_REPORT, 0x0000,
                       pThis->m_ucInterfaceNumber,
                       pThis->m_pReportBuffer, 8) > 0)
    {
        USBGamePadDeviceDecodeReport (pThis);
    }
}