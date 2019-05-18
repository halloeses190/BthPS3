/*
* BthPS3PSM - Windows kernel-mode BTHUSB lower filter driver
* Copyright (C) 2018-2019  Nefarius Software Solutions e.U. and Contributors
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "driver.h"
#include "device.tmh"
#include <usb.h>
#include <usbdi.h>
#include <usbdlib.h>
#include <wdfusb.h>

#ifdef BTHPS3PSM_WITH_CONTROL_DEVICE
extern WDFCOLLECTION   FilterDeviceCollection;
extern WDFWAITLOCK     FilterDeviceCollectionLock;
#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, BthPS3PSM_CreateDevice)
#pragma alloc_text (PAGE, BthPS3PSM_EvtDevicePrepareHardware)
#pragma alloc_text (PAGE, BthPS3PSM_EvtDeviceContextCleanup)
#endif


//
// Called upon device creation
// 
NTSTATUS
BthPS3PSM_CreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    WDF_OBJECT_ATTRIBUTES           deviceAttributes;
    WDFDEVICE                       device;
    NTSTATUS                        status;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;
    PDEVICE_CONTEXT                 deviceContext;


    PAGED_CODE();

    WdfFdoInitSetFilter(DeviceInit);

    //
    // PNP/Power callbacks
    // 
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = BthPS3PSM_EvtDevicePrepareHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    //
    // Device object attributes
    // 
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.EvtCleanupCallback = BthPS3PSM_EvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

    if (NT_SUCCESS(status))
    {
#pragma region Add this device to global collection

#ifdef BTHPS3PSM_WITH_CONTROL_DEVICE

        //
        // Add this device to the FilterDevice collection.
        //
        WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);
        //
        // WdfCollectionAdd takes a reference on the item object and removes
        // it when you call WdfCollectionRemove.
        //
        status = WdfCollectionAdd(FilterDeviceCollection, device);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_QUEUE,
                "WdfCollectionAdd failed with status %!STATUS!",
                status
            );
        }
        WdfWaitLockRelease(FilterDeviceCollectionLock);

        if (!NT_SUCCESS(status)) {
            return status;
        }

#endif

#pragma endregion

        deviceContext = DeviceGetContext(device);

        if (IsDummyDevice(device))
        {
            TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_DEVICE,
                "It appears we're loaded onto a dummy device, remaining silent"
            );

            deviceContext->IsDummyDevice = TRUE;
            return STATUS_SUCCESS;
        }

        //
        // Query for Compatible IDs and opt-out on unsupported devices
        // 
        if (!IsCompatibleDevice(device))
        {
            TraceEvents(TRACE_LEVEL_WARNING,
                TRACE_DEVICE,
                "It appears we're not loaded within a USB stack, aborting initialization"
            );

            return STATUS_INVALID_DEVICE_REQUEST;
        }

#ifndef BTHPS3PSM_WITH_CONTROL_DEVICE
        deviceContext->IsPsmHidControlPatchingEnabled = TRUE;
        deviceContext->IsPsmHidInterruptPatchingEnabled = TRUE;
#else

#pragma region Create control device

        //
        // Create a control device
        //
        status = BthPS3PSM_CreateControlDevice(device);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR,
                TRACE_QUEUE,
                "BthPS3PSM_CreateControlDevice failed with status %!STATUS!",
                status
            );

            return status;
        }

#pragma endregion

#endif

        //
        // Initialize the I/O Package and any Queues
        //
        status = BthPS3PSM_QueueInitialize(device);
    }

    return status;
}

//
// Called upon powering up
// 
NTSTATUS
BthPS3PSM_EvtDevicePrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
)
{
    NTSTATUS                        status = STATUS_SUCCESS;
    WDF_USB_DEVICE_CREATE_CONFIG    usbConfig;
    PDEVICE_CONTEXT                 deviceContext;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");

    deviceContext = DeviceGetContext(Device);

    //
    // We're DUT, don't move!
    // 
    if (deviceContext->IsDummyDevice)
    {
        goto exit;
    }

    //
    // Initialize the USB config context.
    //
    WDF_USB_DEVICE_CREATE_CONFIG_INIT(
        &usbConfig,
        USBD_CLIENT_CONTRACT_VERSION_602
    );

    //
    // Allocate framework USB device object
    // 
    // Since we're a filter we _must not_ blindly
    // call WdfUsb* functions but "abuse" the URBs
    // coming from the upper function driver.
    // 
    status = WdfUsbTargetDeviceCreateWithParameters(
        Device,
        &usbConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &deviceContext->UsbDevice
    );

    if (!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_QUEUE,
            "WdfUsbTargetDeviceCreateWithParameters failed with status %!STATUS!",
            status);
    }

exit:

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");

    return status;
}

//
// Called upon device context clean-up
// 
#pragma warning(push)
#pragma warning(disable:28118) // this callback will run at IRQL=PASSIVE_LEVEL
_Use_decl_annotations_
VOID
BthPS3PSM_EvtDeviceContextCleanup(
    WDFOBJECT Device
)
{
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Entry");

#ifdef BTHPS3PSM_WITH_CONTROL_DEVICE
    
    ULONG   count;

    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);

    count = WdfCollectionGetCount(FilterDeviceCollection);

    if (count == 1)
    {
        //
        // We are the last instance. So let us delete the control-device
        // so that driver can unload when the FilterDevice is deleted.
        // We absolutely have to do the deletion of control device with
        // the collection lock acquired because we implicitly use this
        // lock to protect ControlDevice global variable. We need to make
        // sure another thread doesn't attempt to create while we are
        // deleting the device.
        //
        BthPS3PSM_DeleteControlDevice((WDFDEVICE)Device);
    }

    WdfCollectionRemove(FilterDeviceCollection, Device);

    WdfWaitLockRelease(FilterDeviceCollectionLock);
#else
    UNREFERENCED_PARAMETER(Device);
#endif

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "%!FUNC! Exit");
}
#pragma warning(pop) // enable 28118 again
