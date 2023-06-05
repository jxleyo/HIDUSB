/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:

    usb.c

Abstract:

    Code for handling USB related requests

Author:


Environment:

    kernel mode only

Revision History:

--*/

#include "hidusb_kmdf.h"

#if defined(EVENT_TRACING)
#include "hidusbfx2.tmh"
#else
ULONG DebugLevel = TRACE_LEVEL_INFORMATION;
ULONG DebugFlag = 0xff;
#endif


#ifdef ALLOC_PRAGMA
#pragma alloc_text( INIT, DriverEntry )
#pragma alloc_text( PAGE, HidFx2EvtDeviceAdd)
#pragma alloc_text( PAGE, HidFx2EvtDriverContextCleanup)

#pragma alloc_text( PAGE, SendVendorCommand)
#pragma alloc_text( PAGE, GetVendorData)

#pragma alloc_text(PAGE, HidFx2EvtDevicePrepareHardware)
#pragma alloc_text(PAGE, HidFx2EvtDeviceD0Exit)
#pragma alloc_text(PAGE, HidFx2ConfigContReaderForInterruptEndPoint)
#pragma alloc_text(PAGE, HidFx2ValidateConfigurationDescriptor)
#endif

#define debug_on 1

// 此函数类似于WDM中的PNP_MN_STOP_DEVICE函数，在设备移除时被调用。
// 当个函数被调用时候，设备仍处于工作状态。
NTSTATUS ReleaseHardware(IN WDFDEVICE Device, IN WDFCMRESLIST ResourceListTranslated)
{
    NTSTATUS                             status;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS  configParams;
    PDEVICE_EXTENSION                      pDeviceContext;

    UNREFERENCED_PARAMETER(ResourceListTranslated);

    pDeviceContext = GetDeviceContext(Device);

    // 如果PnpPrepareHardware调用失败,UsbDevice为空；
    // 这时候直接返回即可。
    if (pDeviceContext->UsbDevice == NULL)
        return STATUS_SUCCESS;

    // 取消USB设备的所有IO操作。它将连带取消所有Pipe的IO操作。
    WdfIoTargetStop(WdfUsbTargetDeviceGetIoTarget(pDeviceContext->UsbDevice), WdfIoTargetCancelSentIo);

    // Deconfiguration或者“反配置”
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_DECONFIG(&configParams);
    status = WdfUsbTargetDeviceSelectConfig(
        pDeviceContext->UsbDevice,
        WDF_NO_OBJECT_ATTRIBUTES,
        &configParams);

    RegDebug(L"ReleaseHardware end", NULL, status);
    return STATUS_SUCCESS;
}

// 此函数类似于WDM中的PNP_MN_START_DEVICE函数，紧接着PnpAdd之后被调用。
// 此时PNP管理器经过甄别之后，已经决定将那些系统资源分配给当前设备。
// 参数ResourceList和ResourceListTranslated代表了这些系统资源。
// 当个函数被调用时候，设备已经进入了D0电源状态；函数完成后，设备即正式进入工作状态。
NTSTATUS
HidFx2EvtDevicePrepareHardware(
    IN WDFDEVICE    Device,
    IN WDFCMRESLIST ResourceList,
    IN WDFCMRESLIST ResourceListTranslated
    )
/*++

Routine Description:

    In this callback, the driver does whatever is necessary to make the
    hardware ready to use.  In the case of a USB device, this involves
    reading and selecting descriptors.

Arguments:

    Device - handle to a device

    ResourceList - A handle to a framework resource-list object that
    identifies the raw hardware resourcest

    ResourceListTranslated - A handle to a framework resource-list object
    that identifies the translated hardware resources

Return Value:

    NT status value

--*/
{
    NTSTATUS                            status = STATUS_SUCCESS;
    PDEVICE_EXTENSION                   devContext = NULL;
   // WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    WDF_OBJECT_ATTRIBUTES               attributes;
    PUSB_DEVICE_DESCRIPTOR              usbDeviceDescriptor = NULL;

    WDF_USB_DEVICE_INFORMATION deviceInfo;
    ULONG waitWakeEnable = FALSE;


    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    PAGED_CODE ();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT,
        "HidFx2EvtDevicePrepareHardware Enter\n");

    RegDebug(L"HidFx2EvtDevicePrepareHardware start", NULL, status);

    devContext = GetDeviceContext(Device);

    //
    // Create a WDFUSBDEVICE object. WdfUsbTargetDeviceCreate obtains the
    // USB device descriptor and the first USB configuration descriptor from
    // the device and stores them. It also creates a framework USB interface
    // object for each interface in the device's first configuration.
    //
    // The parent of each USB device object is the driver's framework driver
    // object. The driver cannot change this parent, and the ParentObject
    // member or the WDF_OBJECT_ATTRIBUTES structure must be NULL.
    //
    // We only create device the first time PrepareHardware is called. If
    // the device is restarted by pnp manager for resource rebalance, we
    // will use the same device handle but then select the interfaces again
    // because the USB stack could reconfigure the device on restart.
    //
    if (devContext->UsbDevice == NULL) {
        //WDF_USB_DEVICE_CREATE_CONFIG  Config;

        //WDF_USB_DEVICE_CREATE_CONFIG_INIT(
        //    &Config,
        //    USBD_CLIENT_CONTRACT_VERSION_602//USBD_INTERFACE_VERSION_600//USBD_INTERFACE_VERSION_602//USBD_CLIENT_CONTRACT_VERSION_602
        //);

        //status = WdfUsbTargetDeviceCreateWithParameters(
        //    Device,
        //    &Config,
        //    WDF_NO_OBJECT_ATTRIBUTES,
        //    &devContext->UsbDevice
        //);

        //if (!NT_SUCCESS(status)) {
        //    TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
        //        "WdfUsbTargetDeviceCreate failed 0x%x\n", status);

        //    RegDebug(L"HidFx2EvtDevicePrepareHardware WdfUsbTargetDeviceCreate failed", NULL, status);
        //    return status;
        //}

        status = WdfUsbTargetDeviceCreate(Device,
                                  WDF_NO_OBJECT_ATTRIBUTES,
                                  &devContext->UsbDevice);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                "WdfUsbTargetDeviceCreate failed 0x%x\n", status);

            RegDebug(L"HidFx2EvtDevicePrepareHardware WdfUsbTargetDeviceCreate failed", NULL, status);
            return status;
        }

        devContext->UsbIoTarget = WdfUsbTargetDeviceGetIoTarget(devContext->UsbDevice);
        if (!devContext->UsbIoTarget) {
            RegDebug(L"HidFx2EvtDevicePrepareHardware WdfUsbTargetDeviceGetIoTarget UsbIoTarget failed", NULL, status);
        }

        //
        // TODO: If you are fetching configuration descriptor from device for
        // selecting a configuration or to parse other descriptors, call
        // HidFx2ValidateConfigurationDescriptor
        // to do basic validation on the descriptors before you access them.
        //

    }

    //
    WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);

    status = WdfUsbTargetDeviceRetrieveInformation(
        devContext->UsbDevice,
        &deviceInfo);
    if (NT_SUCCESS(status)) {
        //RegDebug(L"OsrFxEvtDevicePrepareHardware WdfUsbTargetDeviceRetrieveInformation start", NULL, status);
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "IsDeviceHighSpeed: %s\n",
            (deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED) ? "TRUE" : "FALSE");
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
            "IsDeviceSelfPowered: %s\n",
            (deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_SELF_POWERED) ? "TRUE" : "FALSE");

        waitWakeEnable = deviceInfo.Traits &
            WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE;

        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
            "IsDeviceRemoteWakeable: %s\n",
            waitWakeEnable ? "TRUE" : "FALSE");
        //
        // Save these for use later.
        //
        devContext->UsbDeviceTraits = deviceInfo.Traits;
        RegDebug(L"OsrFxEvtDevicePrepareHardware deviceInfo.Traits=", NULL, deviceInfo.Traits);
        RegDebug(L"OsrFxEvtDevicePrepareHardware waitWakeEnable=", NULL, waitWakeEnable);
        RegDebug(L"OsrFxEvtDevicePrepareHardware deviceInfo.UsbdVersionInformation=", NULL, deviceInfo.UsbdVersionInformation.USBDI_Version);

    }
    else {
        RegDebug(L"OsrFxEvtDevicePrepareHardware WdfUsbTargetDeviceRetrieveInformation failed", NULL, status);
        devContext->UsbDeviceTraits = 0;
    }

    // Get the device descriptor and store it in device context
//
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfMemoryCreate(
        &attributes,
        NonPagedPoolNx,
        0,
        sizeof(USB_DEVICE_DESCRIPTOR),
        &devContext->DeviceDescriptor,
        &usbDeviceDescriptor
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfMemoryCreate for Device Descriptor failed %!STATUS!\n",
            status);

        RegDebug(L"HidFx2EvtDevicePrepareHardware WdfMemoryCreate failed", NULL, status);
        return status;
    }

    WdfUsbTargetDeviceGetDeviceDescriptor(
        devContext->UsbDevice,
        usbDeviceDescriptor
    );

    RegDebug(L"HidFx2EvtDevicePrepareHardware WdfUsbTargetDeviceGetDeviceDescriptor=", usbDeviceDescriptor, sizeof(USB_DEVICE_DESCRIPTOR));//

    ////
    //// Select a device configuration by using a
    //// WDF_USB_DEVICE_SELECT_CONFIG_PARAMS structure to specify USB
    //// descriptors, a URB, or handles to framework USB interface objects.
    ////
    //WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE( &configParams);

    //status = WdfUsbTargetDeviceSelectConfig(devContext->UsbDevice,
    //                                    WDF_NO_OBJECT_ATTRIBUTES,
    //                                    &configParams);
    //if(!NT_SUCCESS(status)) {
    //    TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
    //        "WdfUsbTargetDeviceSelectConfig failed %!STATUS!\n",
    //        status);

    //    RegDebug(L"HidFx2EvtDevicePrepareHardware WdfUsbTargetDeviceSelectConfig failed", NULL, status);
    //    return status;
    //}

    //devContext->UsbInterface =
    //            configParams.Types.SingleInterface.ConfiguredUsbInterface;

    ////
    //// Get the Interrupt pipe. There are other endpoints but we are only
    //// interested in interrupt endpoint since our HID data comes from that
    //// endpoint. Another way to get the interrupt endpoint is by enumerating
    //// through all the pipes in a loop and looking for pipe of Interrupt type.
    ////
    //devContext->InterruptPipe = WdfUsbInterfaceGetConfiguredPipe(
    //                                              devContext->UsbInterface,
    //                                              INTERRUPT_ENDPOINT_INDEX,
    //                                              NULL);// pipeInfo

    //if (NULL == devContext->InterruptPipe) {
    //    TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
    //                "Failed to get interrupt pipe info\n");
    //    status = STATUS_INVALID_DEVICE_STATE;

    //    RegDebug(L"HidFx2EvtDevicePrepareHardware WdfUsbInterfaceGetConfiguredPipe failed", NULL, status);
    //    return status;
    //}

    ////
    //// Tell the framework that it's okay to read less than
    //// MaximumPacketSize
    ////
    //WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(devContext->InterruptPipe);

    // Select interface to use
    status = SelectInterruptInterface(Device);
    if (!NT_SUCCESS(status)) {
        RegDebug(L"HidFx2EvtDevicePrepareHardware SelectInterruptInterface failed", NULL, status);
        return status;
    }

    //
    //configure continuous reader
    //
    status = HidFx2ConfigContReaderForInterruptEndPoint(devContext);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT,
        "HidFx2EvtDevicePrepareHardware Exit, Status:0x%x\n", status);

    RegDebug(L"HidFx2EvtDevicePrepareHardware ok", NULL, status);
    return status;
}


NTSTATUS
HidFx2ConfigContReaderForInterruptEndPoint(
    PDEVICE_EXTENSION DeviceContext
    )
/*++

Routine Description:

    This routine configures a continuous reader on the
    interrupt endpoint. It's called from the PrepareHarware event.

Arguments:

    DeviceContext - Pointer to device context structure

Return Value:

    NT status value

--*/
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE ();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT,
        "HidFx2ConfigContReaderForInterruptEndPoint Enter\n");
    //RegDebug(L"HidFx2ConfigContReaderForInterruptEndPoint start", NULL, status);

    size_t TransferLength = 0;
    TransferLength = DeviceContext->pipeMaxPacketSize;// sizeof(PTP_REPORT);

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&contReaderConfig,
                                          HidFx2EvtUsbInterruptPipeReadComplete,
                                          DeviceContext,    // Context
                                          TransferLength);   // TransferLength//sizeof(UCHAR))
    //
    // Reader requests are not posted to the target automatically.
    // Driver must explictly call WdfIoTargetStart to kick start the
    // reader.  In this sample, it's done in D0Entry.
    // By defaut, framework queues two requests to the target
    // endpoint. Driver can configure up to 10 requests with CONFIG macro.
    //


    contReaderConfig.EvtUsbTargetPipeReadersFailed = AmtPtpEvtUsbInterruptReadersFailed;


    status = WdfUsbTargetPipeConfigContinuousReader(DeviceContext->InterruptPipe,
                                                    &contReaderConfig);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                    "HidFx2ConfigContReaderForInterruptEndPoint failed %x\n",
                    status);

        RegDebug(L"HidFx2ConfigContReaderForInterruptEndPoint WdfUsbTargetPipeConfigContinuousReader failed", NULL, status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT,
        "HidFx2ConfigContReaderForInterruptEndPoint Exit, status:0x%x\n", status);

    RegDebug(L"HidFx2ConfigContReaderForInterruptEndPoint end", NULL, status);

    return status;
}


VOID
HidFx2EvtUsbInterruptPipeReadComplete(
    WDFUSBPIPE  Pipe,
    WDFMEMORY   Buffer,
    size_t      NumBytesTransferred,
    WDFCONTEXT  Context
    )
/*++

Routine Description:

    This the completion routine of the continuous reader. This can
    called concurrently on multiprocessor system if there are
    more than one readers configured. So make sure to protect
    access to global resources.

Arguments:

    Pipe - Handle to WDF USB pipe object

    Buffer - This buffer is freed when this call returns.
             If the driver wants to delay processing of the buffer, it
             can take an additional referrence.

    NumBytesTransferred - number of bytes of data that are in the read buffer.

    Context - Provided in the WDF_USB_CONTINUOUS_READER_CONFIG_INIT macro

Return Value:

    NT status value

--*/
{
    PDEVICE_EXTENSION  devContext = Context;

    UNREFERENCED_PARAMETER(devContext);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(NumBytesTransferred);
    UNREFERENCED_PARAMETER(Pipe);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT,
        "HidFx2EvtUsbInterruptPipeReadComplete Enter\n");

    if (!ioReady) {
        return;
    }

    runtimes_ioRead++;//经过测试会有读取完成
    //RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete start", NULL, runtimes_ioctl_count);
    //
    // Make sure that there is data in the read packet.  Depending on the device
    // specification, it is possible for it to return a 0 length read in
    // certain conditions.
    //

    if (NumBytesTransferred == 0) {//测试结果恒定为5,数据为鼠标集合
        TraceEvents(TRACE_LEVEL_WARNING, DBG_INIT,
                    "HidFx2EvtUsbInterruptPipeReadComplete Zero length read "
                    "occured on the Interrupt Pipe's Continuous Reader\n"
                    );
        KdPrint(("HidFx2EvtUsbInterruptPipeReadComplete NumBytesTransferred err %x", runtimes_ioRead));
        RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete NumBytesTransferred err", NULL, runtimes_ioRead);
        return;
    }

    //if (runtimes_ioRead ==111) {//runtimes_ioRead++;//经过测试会有读取完成，Regdebug导致蓝屏
    //    RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete NumBytesTransferred=", NULL, (ULONG)NumBytesTransferred);
    //    return;
    //}


    //size_t headerSize = (unsigned int)devContext->DeviceInfo->tp_header;
    //size_t fingerprintSize = (unsigned int)devContext->DeviceInfo->tp_fsize;


    //if (NumBytesTransferred < headerSize || (NumBytesTransferred - headerSize) % fingerprintSize != 0) {
    //    RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete NumBytesTransferred err", NULL, NumBytesTransferred);
    //    return;
    //}


    //if (NumBytesTransferred < sizeof(PTP_REPORT)) {
    //    RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete NumBytesTransferred err", NULL, (ULONG)NumBytesTransferred);
    //    return;
    //}

    // Retrieve packet
    UCHAR* TouchBuffer = NULL;
    TouchBuffer = WdfMemoryGetBuffer(
        Buffer,
        NULL
    );
    if (TouchBuffer == NULL) {
        KdPrint(("HidFx2EvtUsbInterruptPipeReadComplete WdfMemoryGetBuffer err %x", runtimes_ioRead));
        return;
    }

    RtlCopyMemory(devContext->TouchData, TouchBuffer, NumBytesTransferred);//5
    devContext->bReadCompletion = TRUE;

    KdPrint(("HidFx2EvtUsbInterruptPipeReadComplete TouchBuffer= 0x%x,0x%x,0x%x,0x%x,0x%x\n", \
        devContext->TouchData[0], devContext->TouchData[1], devContext->TouchData[2], devContext->TouchData[3], devContext->TouchData[4]));


    //if (!WdfTimerStart(devContext->DebounceTimer,WDF_REL_TIMEOUT_IN_MS(100))){//SWICTHPACK_DEBOUNCE_TIME_IN_MS//100
    //    //RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete WdfTimerStart err 0x%x\n", NULL, runtimes_ioRead);
    //    KdPrint(("HidFx2EvtUsbInterruptPipeReadComplete WdfTimerStart err 0x%x\n", runtimes_ioRead));
    //}

    //DispatchReadRequest(devContext);

    KdPrint(("HidFx2EvtUsbInterruptPipeReadComplete end %x", runtimes_ioRead));
    //RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete end", NULL, Status);

}



NTSTATUS
HidFx2EvtDeviceD0Entry(
    IN  WDFDEVICE Device,
    IN  WDF_POWER_DEVICE_STATE PreviousState
    )
/*++

Routine Description:

    EvtDeviceD0Entry event callback must perform any operations that are
    necessary before the specified device is used.  It will be called every
    time the hardware needs to be (re-)initialized.

    This function is not marked pageable because this function is in the
    device power up path. When a function is marked pagable and the code
    section is paged out, it will generate a page fault which could impact
    the fast resume behavior because the client driver will have to wait
    until the system drivers can service this page fault.

    This function runs at PASSIVE_LEVEL, even though it is not paged.  A
    driver can optionally make this function pageable if DO_POWER_PAGABLE
    is set.  Even if DO_POWER_PAGABLE isn't set, this function still runs
    at PASSIVE_LEVEL.  In this case, though, the function absolutely must
    not do anything that will cause a page fault.

Arguments:

    Device - Handle to a framework device object.

    PreviousState - Device power state which the device was in most recently.
        If the device is being newly started, this will be
        PowerDeviceUnspecified.

Return Value:

    NTSTATUS

--*/
{
    PDEVICE_EXTENSION   devContext = NULL;
    NTSTATUS            status = STATUS_SUCCESS;

    devContext = GetDeviceContext(Device);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_PNP,
        "HidFx2EvtDeviceD0Entry Enter - coming from %s\n",
                DbgDevicePowerString(PreviousState));

    RegDebug(L"HidFx2EvtDeviceD0Entry start", NULL, status);
    //
    // 
    devContext->isTargetStarted = FALSE;
    // 
    devContext->PtpInputModeOn = FALSE;
    devContext->GetStringStep = 0;

    ioReady = FALSE;
    runtimes_ioctl_count = 0;
    runtimes_ioRead = 0;
    runtimes_timer = 0;

    RtlZeroMemory(&devContext->TouchData, 30);//
    devContext->bReadCompletion = FALSE;

    //KeInitializeEvent(&devContext->waitReadCompletionEvent,
    //    NotificationEvent,//SynchronizationEvent//NotificationEvent
    //    FALSE);

    //KeResetEvent(&devContext->waitReadCompletionEvent);



    WDFUSBPIPE pipe;
    WDF_USB_PIPE_INFORMATION            pipeInfo;
    //pipe = WdfUsbInterfaceGetConfiguredPipe(devContext->UsbInterface,
    //    0, //PipeIndex,
    //    &pipeInfo
    //);

    pipe = devContext->InterruptPipe;
    WdfUsbTargetPipeGetInformation(pipe, &pipeInfo);

    RegDebug(L"HidFx2EvtDeviceD0Entry check pipeMaxPacketSize=", NULL, pipeInfo.MaximumPacketSize);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.MaximumTransferSize=", NULL, pipeInfo.MaximumTransferSize);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.EndpointAddress=", NULL, pipeInfo.EndpointAddress);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.Interval=", NULL, pipeInfo.Interval);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.SettingIndex=", NULL, pipeInfo.SettingIndex);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.PipeType=", NULL, pipeInfo.PipeType);

    //status = StartAllPipes(devContext); 

        //
    // Start the target. This will start the continuous reader
    //
    WDFIOTARGET UsbTarget = WdfUsbTargetPipeGetIoTarget(devContext->InterruptPipe);
    if (UsbTarget) {
        status = WdfIoTargetStart(UsbTarget);//WdfUsbTargetPipeGetIoTarget(devContext->InterruptPipe);
        if (NT_SUCCESS(status)) {
            devContext->isTargetStarted = TRUE;
            RegDebug(L"HidFx2EvtDeviceD0Entry WdfIoTargetStart ok", NULL, status);
        }
    }

    TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
        "HidFx2EvtDeviceD0Entry Exit, status: 0x%x\n", status);

    RegDebug(L"HidFx2EvtDeviceD0Entry end", NULL, status);
    return status;
}


NTSTATUS
HidFx2EvtDeviceD0Exit(
    IN  WDFDEVICE Device,
    IN  WDF_POWER_DEVICE_STATE TargetState
    )
/*++

Routine Description:

    This routine undoes anything done in EvtDeviceD0Entry.  It is called
    whenever the device leaves the D0 state, which happens when the device is
    stopped, when it is removed, and when it is powered off.

    The device is still in D0 when this callback is invoked, which means that
    the driver can still touch hardware in this routine.


    EvtDeviceD0Exit event callback must perform any operations that are
    necessary before the specified device is moved out of the D0 state.  If the
    driver needs to save hardware state before the device is powered down, then
    that should be done here.

    This function runs at PASSIVE_LEVEL, though it is generally not paged.  A
    driver can optionally make this function pageable if DO_POWER_PAGABLE is set.

    Even if DO_POWER_PAGABLE isn't set, this function still runs at
    PASSIVE_LEVEL.  In this case, though, the function absolutely must not do
    anything that will cause a page fault.

Arguments:

    Device - Handle to a framework device object.

    TargetState - Device power state which the device will be put in once this
        callback is complete.

Return Value:

    Success implies that the device can be used.  Failure will result in the
    device stack being torn down.

--*/
{
    PDEVICE_EXTENSION         devContext;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_PNP,
        "HidFx2EvtDeviceD0Exit Enter- moving to %s\n",
          DbgDevicePowerString(TargetState));

    devContext = GetDeviceContext(Device);
    NTSTATUS status = STATUS_SUCCESS;

    //status = WdfUsbTargetPipeAbortSynchronously(
//devContext->InterruptPipe,
//WDF_NO_HANDLE,
//NULL
//);

    WDFIOTARGET UsbTarget = WdfUsbTargetPipeGetIoTarget(devContext->InterruptPipe);
    if (UsbTarget) {
        WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(// 如果还有未完成的IO操作，都Cancel掉。
            devContext->InterruptPipe), WdfIoTargetCancelSentIo);

        //status = WdfUsbTargetPipeAbortSynchronously(
        //    devContext->InterruptPipe,
        //    WDF_NO_HANDLE,
        //    NULL
        //);

        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "HidFx2EvtDeviceD0Exit Exit\n");

        devContext->isTargetStarted = FALSE;
        RegDebug(L"HidFx2EvtDeviceD0Exit WdfIoTargetStop ok", NULL, runtimes_ioRead);
    }



        status= StopAllPipes(devContext);

        // 完成在手动队列中的所有未完成Request。
    // 如果Queue处于未启动状态，会返回STATUS_WDF_PAUSED；
    // 如果已启动，则会挨个取得其Entry，直到返回STATUS_NO_MORE_ENTRIES。	
        WDFREQUEST Request = NULL;
        do {
            status = WdfIoQueueRetrieveNextRequest(devContext->InterruptMsgQueue, &Request);

            if (NT_SUCCESS(status))
            {
                WdfRequestComplete(Request, STATUS_SUCCESS);
            }
        } while (status != STATUS_NO_MORE_ENTRIES && status != STATUS_WDF_PAUSED);


    RegDebug(L"HidFx2EvtDeviceD0Exit end", NULL, 1);
    return status;
}



USBD_STATUS
HidFx2ValidateConfigurationDescriptor(
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigDesc,
    IN ULONG BufferLength,
    _Inout_  PUCHAR *Offset
    )
/*++

Routine Description:

    Validates a USB Configuration Descriptor

Parameters:

    ConfigDesc: Pointer to the entire USB Configuration descriptor returned by the device

    BufferLength: Known size of buffer pointed to by ConfigDesc (Not wTotalLength)

    Offset: if the USBD_STATUS returned is not USBD_STATUS_SUCCESS, offet will
        be set to the address within the ConfigDesc buffer where the failure occured.

Return Value:

    USBD_STATUS
    Success implies the configuration descriptor is valid.

--*/
{


    USBD_STATUS status = USBD_STATUS_SUCCESS;
    USHORT ValidationLevel = 3;

    PAGED_CODE();

    //
    // Call USBD_ValidateConfigurationDescriptor to validate the descriptors which are present in this supplied configuration descriptor.
    // USBD_ValidateConfigurationDescriptor validates that all descriptors are completely contained within the configuration descriptor buffer.
    // It also checks for interface numbers, number of endpoints in an interface etc.
    // Please refer to msdn documentation for this function for more information.
    //

    status = USBD_ValidateConfigurationDescriptor( ConfigDesc, BufferLength , ValidationLevel , Offset , POOL_TAG );
    if (!(NT_SUCCESS (status)) ){
        return status;
    }

    //
    // TODO: You should validate the correctness of other descriptors which are not taken care by USBD_ValidateConfigurationDescriptor
    // Check that all such descriptors have size >= sizeof(the descriptor they point to)
    // Check for any association between them if required
    //

    return status;
}


PCHAR
DbgDevicePowerString(
    IN WDF_POWER_DEVICE_STATE Type
    )
/*++

Updated Routine Description:
    DbgDevicePowerString does not change in this stage of the function driver.

--*/
{
    switch (Type)
    {
    case WdfPowerDeviceInvalid:
        return "WdfPowerDeviceInvalid";
    case WdfPowerDeviceD0:
        return "WdfPowerDeviceD0";
    case WdfPowerDeviceD1:
        return "WdfPowerDeviceD1";
    case WdfPowerDeviceD2:
        return "WdfPowerDeviceD2";
    case WdfPowerDeviceD3:
        return "WdfPowerDeviceD3";
    case WdfPowerDeviceD3Final:
        return "WdfPowerDeviceD3Final";
    case WdfPowerDevicePrepareForHibernation:
        return "WdfPowerDevicePrepareForHibernation";
    case WdfPowerDeviceMaximum:
        return "WdfPowerDeviceMaximum";
    default:
        return "UnKnown Device Power State";
    }
}




VOID
HidFx2EvtInternalDeviceControl(
    IN WDFQUEUE     Queue,
    IN WDFREQUEST   Request,
    IN size_t       OutputBufferLength,
    IN size_t       InputBufferLength,
    IN ULONG        IoControlCode
)
/*++

Routine Description:

    This event is called when the framework receives
    IRP_MJ_INTERNAL DEVICE_CONTROL requests from the system.

Arguments:

    Queue - Handle to the framework queue object that is associated
            with the I/O request.
    Request - Handle to a framework request object.

    OutputBufferLength - length of the request's output buffer,
                        if an output buffer is available.
    InputBufferLength - length of the request's input buffer,
                        if an input buffer is available.

    IoControlCode - the driver-defined or system-defined I/O control code
                    (IOCTL) that is associated with the request.
Return Value:

    VOID

--*/

{
    NTSTATUS            status = STATUS_SUCCESS;
    WDFDEVICE           device;
    PDEVICE_EXTENSION   devContext = NULL;
    ULONG               bytesReturned = 0;

    UNREFERENCED_PARAMETER(bytesReturned);

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    
    device = WdfIoQueueGetDevice(Queue);
    PDEVICE_OBJECT pDevObj = WdfDeviceWdmGetDeviceObject(device);
    UNREFERENCED_PARAMETER(pDevObj);
    //RegDebug(L"HidFx2EvtInternalDeviceControl pDevObj=", pDevObj->DriverObject->DriverName.Buffer, pDevObj->DriverObject->DriverName.Length);
    //devContext = GetDeviceContext(device);
    //RegDebug(L"HidFx2EvtInternalDeviceControl devContext->DeviceObject=", devContext->DeviceObject->DriverObject->DriverName.Buffer, devContext->DeviceObject->DriverObject->DriverName.Length);

    runtimes_ioctl_count++;
    //if (runtimes_ioControl == 1) {
    //    RegDebug(L"IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST IoControlCode", NULL, IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST);//0xb002b
    //    RegDebug(L"IOCTL_HID_GET_DEVICE_DESCRIPTOR IoControlCode", NULL, IOCTL_HID_GET_DEVICE_DESCRIPTOR);//0xb0003
    //    RegDebug(L"IOCTL_HID_GET_REPORT_DESCRIPTOR IoControlCode", NULL, IOCTL_HID_GET_REPORT_DESCRIPTOR);//0xb0007
    //    RegDebug(L"IOCTL_HID_GET_DEVICE_ATTRIBUTES IoControlCode", NULL, IOCTL_HID_GET_DEVICE_ATTRIBUTES);//0xb0027
    //    RegDebug(L"IOCTL_HID_READ_REPORT IoControlCode", NULL, IOCTL_HID_READ_REPORT);//0xb000b
    //    RegDebug(L"IOCTL_HID_WRITE_REPORT IoControlCode", NULL, IOCTL_HID_WRITE_REPORT);//0xb000f
    //    RegDebug(L"IOCTL_HID_GET_STRING IoControlCode", NULL, IOCTL_HID_GET_STRING);//0xb0013
    //    RegDebug(L"IOCTL_HID_GET_FEATURE IoControlCode", NULL, IOCTL_HID_GET_FEATURE);//0xb0192
    //    RegDebug(L"IOCTL_HID_SET_FEATURE IoControlCode", NULL, IOCTL_HID_SET_FEATURE);//0xb0191
    //    RegDebug(L"IOCTL_HID_GET_INPUT_REPORT IoControlCode", NULL, IOCTL_HID_GET_INPUT_REPORT);//0xb01a2
    //    RegDebug(L"IOCTL_HID_SET_OUTPUT_REPORT IoControlCode", NULL, IOCTL_HID_SET_OUTPUT_REPORT);//0xb0195
    //    RegDebug(L"IOCTL_HID_DEVICERESET_NOTIFICATION IoControlCode", NULL, IOCTL_HID_DEVICERESET_NOTIFICATION);//0xb0233
    //}

    RegDebug(L"HidFx2EvtInternalDeviceControl runtimes_ioctl_count", NULL, runtimes_ioctl_count);
    RegDebug(L"HidFx2EvtInternalDeviceControl IoControlCode", NULL, IoControlCode);
    RegDebug(L"HidFx2EvtInternalDeviceControl runtimes_ioRead", NULL, runtimes_ioRead);


    ////其他的IoControlCode交给下层处理
    //if (1) {
    //    Filter_DispatchPassThrough(Request, WdfDeviceGetIoTarget(device));
    //    return;
    //}


    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTL,
        "%s, Queue:0x%p, Request:0x%p\n",
        DbgHidInternalIoctlString(IoControlCode),
        Queue,
        Request
    );

    //
    // Please note that HIDCLASS provides the buffer in the Irp->UserBuffer
    // field irrespective of the ioctl buffer type. However, framework is very
    // strict about type checking. You cannot get Irp->UserBuffer by using
    // WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
    // internal ioctl. So depending on the ioctl code, we will either
    // use retreive function or escape to WDM to get the UserBuffer.
    //

    switch (IoControlCode) {

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        //RegDebug(L"IOCTL_HID_GET_DEVICE_DESCRIPTOR", NULL, runtimes_ioctl_count);
        WDFMEMORY memory;
        status = WdfRequestRetrieveOutputMemory(Request, &memory);
        if (!NT_SUCCESS(status)) {
            RegDebug(L"IOCTL_HID_GET_DEVICE_DESCRIPTOR WdfRequestRetrieveOutputMemory err", NULL, runtimes_ioctl_count);
            break;
        }

        //USHORT ReportDescriptorLength = pDevContext->HidSettings.ReportDescriptorLength;//设置描述符长度
        //HID_DESCRIPTOR HidDescriptor = {0x09, 0x21, 0x0100, 0x00, 0x01, { 0x22, ReportDescriptorLength }  // HidReportDescriptor
        //};
        //RegDebug(L"IOCTL_HID_GET_DEVICE_DESCRIPTOR HidDescriptor=", &HidDescriptor, HidDescriptor.bLength);

        status = WdfMemoryCopyFromBuffer(memory, 0, (PVOID)&DefaultHidDescriptor, DefaultHidDescriptor.bLength);//DefaultHidDescriptor//HidDescriptor
        if (!NT_SUCCESS(status)) {
            RegDebug(L"IOCTL_HID_GET_DEVICE_DESCRIPTOR WdfMemoryCopyFromBuffer err", NULL, runtimes_ioctl_count);
            break;
        }
        ////
        WdfRequestSetInformation(Request, DefaultHidDescriptor.bLength);//DefaultHidDescriptor//HidDescriptor


        //
        // Retrieves the device's HID descriptor.
        //
        //status = HidFx2GetHidDescriptor(device, Request);
        break;
    }
        

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        //
        //Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
        //
        status = HidFx2GetDeviceAttributes(Request);
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
        //RegDebug(L"IOCTL_HID_GET_REPORT_DESCRIPTOR", NULL, runtimes_ioctl_count);

        WDFMEMORY memory2;
        status = WdfRequestRetrieveOutputMemory(Request, &memory2);
        if (!NT_SUCCESS(status)) {
            RegDebug(L"OnInternalDeviceIoControl WdfRequestRetrieveOutputMemory err", NULL, runtimes_ioctl_count);
            break;
        }

        //PVOID outbuf = pDevContext->pReportDesciptorData;
        //LONG outlen = pDevContext->HidSettings.ReportDescriptorLength;//设置描述符长度
        //RegDebug(L"IOCTL_HID_GET_REPORT_DESCRIPTOR HidDescriptor=", pDevContext->pReportDesciptorData, outlen);

        PVOID outbuf = (PVOID)ParallelMode_PtpReportDescriptor;//(PVOID)ParallelMode_PtpReportDescriptor //(PVOID)MouseReportDescriptor//(PVOID)SingleFingerHybridMode_PtpReportDescriptor
        LONG outlen = DefaultHidDescriptor.DescriptorList[0].wReportLength;

        status = WdfMemoryCopyFromBuffer(memory2, 0, outbuf, outlen);
        if (!NT_SUCCESS(status)) {
            RegDebug(L"IOCTL_HID_GET_REPORT_DESCRIPTOR WdfMemoryCopyFromBuffer err", NULL, runtimes_ioctl_count);
            break;
        }
        WdfRequestSetInformation(Request, outlen);

        //
        //Obtains the report descriptor for the HID device.
        //
        //status = HidFx2GetReportDescriptor(device, Request);
        break;
    }
    case IOCTL_HID_READ_REPORT: {
        RegDebug(L"HidFx2EvtInternalDeviceControl IOCTL_HID_READ_REPORT", NULL, runtimes_ioctl_count);
        DbgPrint("HidFx2EvtInternalDeviceControl IOCTL_HID_READ_REPORT runtimes_ioRead 0x%x\n",runtimes_ioctl_count);
        //
              // Returns a report from the device into a class driver-supplied buffer.
              // For now queue the request to the manual queue. The request will
              // be retrived and completd when continuous reader reads new data
              // from the device.
              //

        //
        status = WdfRequestForwardToIoQueue(Request, devContext->InterruptMsgQueue);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
                "WdfRequestForwardToIoQueue failed with status: 0x%x\n", status);

            RegDebug(L"HidFx2EvtInternalDeviceControl IOCTL_HID_READ_REPORT WdfRequestForwardToIoQueue failed", NULL, status);
            DbgPrint("HidFx2EvtInternalDeviceControl IOCTL_HID_READ_REPORT WdfRequestForwardToIoQueuefailed\n");

            WdfRequestComplete(Request, status);
        }

        ////
        //DispatchRead(devContext);

        return;

        //
        // This feature is only supported on WinXp and later. Compiling in W2K
        // build environment will fail without this conditional preprocessor statement.
        //
    }
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:

        //
        // Hidclass sends this IOCTL for devices that have opted-in for Selective
        // Suspend feature. This feature is enabled by adding a registry value
        // "SelectiveSuspendEnabled" = 1 in the hardware key through inf file
        // (see hidusbfx2.inf). Since hidclass is the power policy owner for
        // this stack, it controls when to send idle notification and when to
        // cancel it. This IOCTL is passed to USB stack. USB stack pends it.
        // USB stack completes the request when it determines that the device is
        // idle. Hidclass's idle notification callback get called that requests a
        // wait-wake Irp and subsequently powers down the device.
        // The device is powered-up either when a handle is opened for the PDOs
        // exposed by hidclass, or when usb stack completes wait
        // wake request. In the first case, hidclass cancels the notification
        // request (pended with usb stack), cancels wait-wake Irp and powers up
        // the device. In the second case, an external wake event triggers completion
        // of wait-wake irp and powering up of device.
        //
        status = HidFx2SendIdleNotification(Request);

        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
                "SendIdleNotification failed with status: 0x%x\n", status);

            WdfRequestComplete(Request, status);
        }

        return;

    case IOCTL_HID_SET_FEATURE:
        //
        // This sends a HID class feature report to a top-level collection of
        // a HID class device.
        //
        //status = HidFx2SetFeature(Request);
        WdfRequestComplete(Request, status);
        RegDebug(L"IOCTL_HID_SET_FEATURE ok", NULL, runtimes_ioctl_count);

        ioReady = TRUE;

        return;

    case IOCTL_HID_GET_FEATURE:
        status = PtpReportFeatures(
            device,
            Request
        );

        WdfRequestComplete(Request, status);


        //
        //// Get a HID class feature report from a top-level collection of
        //// a HID class device.
        ////
        //status = HidFx2GetFeature(Request, &bytesReturned);
        //WdfRequestCompleteWithInformation(Request, status, bytesReturned);
        return;

    case IOCTL_HID_WRITE_REPORT:
        RegDebug(L"IOCTL_HID_WRITE_REPORT ok", NULL, runtimes_ioctl_count);
        //
        //Transmits a class driver-supplied report to the device.
        //
    case IOCTL_HID_GET_STRING:
        RegDebug(L"HidFx2EvtInternalDeviceControl IOCTL_HID_GET_STRING", NULL, IoControlCode);
        //
        // Requests that the HID minidriver retrieve a human-readable string
        // for either the manufacturer ID, the product ID, or the serial number
        // from the string descriptor of the device. The minidriver must send
        // a Get String Descriptor request to the device, in order to retrieve
        // the string descriptor, then it must extract the string at the
        // appropriate index from the string descriptor and return it in the
        // output buffer indicated by the IRP. Before sending the Get String
        // Descriptor request, the minidriver must retrieve the appropriate
        // index for the manufacturer ID, the product ID or the serial number
        // from the device extension of a top level collection associated with
        // the device.
        //
        status = HidGetString(devContext, Request);//代码会死机
        break;
    case IOCTL_HID_ACTIVATE_DEVICE:
        RegDebug(L"HidFx2EvtInternalDeviceControl IOCTL_HID_ACTIVATE_DEVICE", NULL, IoControlCode);
        //
        // Makes the device ready for I/O operations.
        //
    case IOCTL_HID_DEACTIVATE_DEVICE:
        RegDebug(L"HidFx2EvtInternalDeviceControl IOCTL_HID_DEACTIVATE_DEVICE", NULL, IoControlCode);
        //
        // Causes the device to cease operations and terminate all outstanding
        // I/O requests.
        //
    default:
        status = STATUS_NOT_SUPPORTED;
        RegDebug(L"HidFx2EvtInternalDeviceControl STATUS_NOT_SUPPORTED", NULL, IoControlCode);
        break;
    }

    WdfRequestComplete(Request, status);
    RegDebug(L"HidFx2EvtInternalDeviceControl end", NULL, runtimes_ioctl_count);
    return;
}



NTSTATUS
SendVendorCommand(
    IN WDFDEVICE Device,
    IN UCHAR VendorCommand,
    IN PUCHAR CommandData
)
/*++

Routine Description

    This routine sets the state of the Feature: in this
    case Segment Display on the USB FX2 board.

Arguments:

    Request - Wdf Request

Return Value:

    NT status value

--*/
{
    NTSTATUS                     status = STATUS_SUCCESS;
    ULONG                        bytesTransferred = 0;
    PDEVICE_EXTENSION            pDevContext = NULL;
    WDF_MEMORY_DESCRIPTOR        memDesc;
    WDF_USB_CONTROL_SETUP_PACKET controlSetupPacket;
    WDF_REQUEST_SEND_OPTIONS     sendOptions;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL, "SendVendorCommand Enter\n");

    pDevContext = GetDeviceContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTL,
        "SendVendorCommand: Command:0x%x, data: 0x%x\n",
        VendorCommand, *CommandData);

    //
    // set the segment state on the USB device
    //
    // Send the I/O with a timeout. We do that because we send the
    // I/O in the context of the user thread and if it gets stuck, it would
    // prevent the user process from existing.
    //
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions,
        WDF_REQUEST_SEND_OPTION_TIMEOUT);

    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOptions,
        WDF_REL_TIMEOUT_IN_SEC(5));

    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(&controlSetupPacket,
        BmRequestHostToDevice,
        BmRequestToDevice,
        VendorCommand, // Request
        0, // Value
        0); // Index

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDesc,
        CommandData,
        sizeof(UCHAR));

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        pDevContext->UsbDevice,
        WDF_NO_HANDLE, // Optional WDFREQUEST
        &sendOptions, // PWDF_REQUEST_SEND_OPTIONS
        &controlSetupPacket,
        &memDesc,
        &bytesTransferred
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "SendtVendorCommand: Failed to set Segment Display state - 0x%x \n",
            status);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL, "SendVendorCommand Exit\n");

    RegDebug(L"SendVendorCommand end", NULL, status);

    return status;
}

NTSTATUS
GetVendorData(
    IN WDFDEVICE Device,
    IN UCHAR VendorCommand,
    IN PUCHAR CommandData
)
/*++

Routine Description

    This routine sets the state of the Feature: in this
    case Segment Display on the USB FX2 board.

Arguments:

    Request - Wdf Request

Return Value:

    NT status value

--*/
{
    NTSTATUS                     status = STATUS_SUCCESS;
    ULONG                        bytesTransferred = 0;
    PDEVICE_EXTENSION            pDevContext = NULL;
    WDF_MEMORY_DESCRIPTOR        memDesc;
    WDF_USB_CONTROL_SETUP_PACKET controlSetupPacket;
    WDF_REQUEST_SEND_OPTIONS     sendOptions;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL, "GetVendorData Enter\n");

    pDevContext = GetDeviceContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTL,
        "GetVendorData: Command:0x%x, data: 0x%x\n",
        VendorCommand, *CommandData);

    //
    // Get the display state from the USB device
    //
    // Send the I/O with a timeout. We do that because we send the
    // I/O in the context of the user thread and if it gets stuck, it would
    // prevent the user process from existing.
    //
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions,
        WDF_REQUEST_SEND_OPTION_TIMEOUT);

    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOptions,
        WDF_REL_TIMEOUT_IN_SEC(5));

    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(&controlSetupPacket,
        BmRequestDeviceToHost,
        BmRequestToDevice,
        VendorCommand, // Request
        0, // Value
        0); // Index

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDesc,
        CommandData,
        sizeof(UCHAR));

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        pDevContext->UsbDevice,
        WDF_NO_HANDLE, // Optional WDFREQUEST
        &sendOptions, // PWDF_REQUEST_SEND_OPTIONS
        &controlSetupPacket,
        &memDesc,
        &bytesTransferred
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "GetVendorData: Failed to get state - 0x%x \n",
            status);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTL,
            "GetVendorData: Command:0x%x, data after command: 0x%x\n",
            VendorCommand, *CommandData);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL, "GetVendorData Exit\n");

    RegDebug(L"GetVendorData end", NULL, status);

    return status;
}


NTSTATUS
HidFx2GetHidDescriptor(
    IN WDFDEVICE Device,
    IN WDFREQUEST Request
)
/*++

Routine Description:

    Finds the HID descriptor and copies it into the buffer provided by the
    Request.

Arguments:

    Device - Handle to WDF Device Object

    Request - Handle to request object

Return Value:

    NT status code.

--*/
{
    NTSTATUS            status = STATUS_SUCCESS;
    size_t              bytesToCopy = 0;
    WDFMEMORY           memory;

    UNREFERENCED_PARAMETER(Device);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL,
        "HidFx2GetHidDescriptor Entry\n");

    //
    // This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
    // will correctly retrieve buffer from Irp->UserBuffer.
    // Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
    // field irrespective of the ioctl buffer type. However, framework is very
    // strict about type checking. You cannot get Irp->UserBuffer by using
    // WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
    // internal ioctl.
    //
    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "WdfRequestRetrieveOutputMemory failed 0x%x\n", status);
        RegDebug(L"HidFx2GetHidDescriptor WdfRequestRetrieveOutputMemory failed", NULL, status);
        return status;
    }

    //
    // Use hardcoded "HID Descriptor"
    //
    bytesToCopy = DefaultHidDescriptor.bLength;

    if (bytesToCopy == 0) {
        status = STATUS_INVALID_DEVICE_STATE;
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "G_DefaultHidDescriptor is zero, 0x%x\n", status);
        RegDebug(L"HidFx2GetHidDescriptor G_DefaultHidDescriptor is zero", NULL, status);
        return status;
    }

    status = WdfMemoryCopyFromBuffer(memory,
        0, // Offset
        (PVOID)&DefaultHidDescriptor,
        bytesToCopy);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "WdfMemoryCopyFromBuffer failed 0x%x\n", status);
        RegDebug(L"HidFx2GetHidDescriptor WdfMemoryCopyFromBuffer failed", NULL, status);
        return status;
    }

    //
    // Report how many bytes were copied
    //
    WdfRequestSetInformation(Request, bytesToCopy);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL,
        "HidFx2GetHidDescriptor Exit = 0x%x\n", status);
    RegDebug(L"HidFx2GetHidDescriptor end", NULL, runtimes_ioctl_count);
    return status;
}

NTSTATUS
HidFx2GetReportDescriptor(
    IN WDFDEVICE Device,
    IN WDFREQUEST Request
)
/*++

Routine Description:

    Finds the Report descriptor and copies it into the buffer provided by the
    Request.

Arguments:

    Device - Handle to WDF Device Object

    Request - Handle to request object

Return Value:

    NT status code.

--*/
{
    NTSTATUS            status = STATUS_SUCCESS;
    ULONG_PTR           bytesToCopy;
    WDFMEMORY           memory;

    UNREFERENCED_PARAMETER(Device);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL,
        "HidFx2GetReportDescriptor Entry\n");

    //
    // This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
    // will correctly retrieve buffer from Irp->UserBuffer.
    // Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
    // field irrespective of the ioctl buffer type. However, framework is very
    // strict about type checking. You cannot get Irp->UserBuffer by using
    // WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
    // internal ioctl.
    //
    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "WdfRequestRetrieveOutputMemory failed 0x%x\n", status);
        RegDebug(L"HidFx2GetReportDescriptor WdfRequestRetrieveOutputMemory failed", NULL, status);
        return status;
    }

    //
    // Use hardcoded Report descriptor
    //
    bytesToCopy = DefaultHidDescriptor.DescriptorList[0].wReportLength;

    if (bytesToCopy == 0) {
        status = STATUS_INVALID_DEVICE_STATE;
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "G_DefaultHidDescriptor's reportLenght is zero, 0x%x\n", status);
        RegDebug(L"HidFx2GetReportDescriptor G_DefaultHidDescriptor's reportLenght is zero", NULL, status);
        return status;
    }

    status = WdfMemoryCopyFromBuffer(memory,
        0,
        (PVOID)&DefaultHidDescriptor,
        bytesToCopy);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "WdfMemoryCopyFromBuffer failed 0x%x\n", status);
        RegDebug(L"HidFx2GetReportDescriptor WdfMemoryCopyFromBuffer failed", NULL, status);
        return status;
    }

    //
    // Report how many bytes were copied
    //
    WdfRequestSetInformation(Request, bytesToCopy);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL,
        "HidFx2GetReportDescriptor Exit = 0x%x\n", status);
    RegDebug(L"HidFx2GetReportDescriptor end", NULL, runtimes_ioctl_count);
    return status;
}


NTSTATUS
HidFx2GetDeviceAttributes(
    IN WDFREQUEST Request
)
/*++

Routine Description:

    Fill in the given struct _HID_DEVICE_ATTRIBUTES

Arguments:

    Request - Pointer to Request object.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                 status = STATUS_SUCCESS;
    PHID_DEVICE_ATTRIBUTES   deviceAttributes = NULL;
    PUSB_DEVICE_DESCRIPTOR   usbDeviceDescriptor = NULL;
    PDEVICE_EXTENSION        deviceInfo = NULL;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL,
        "HidFx2GetDeviceAttributes Entry\n");

    deviceInfo = GetDeviceContext(WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request)));

    //
    // This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
    // will correctly retrieve buffer from Irp->UserBuffer.
    // Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
    // field irrespective of the ioctl buffer type. However, framework is very
    // strict about type checking. You cannot get Irp->UserBuffer by using
    // WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
    // internal ioctl.
    //
    status = WdfRequestRetrieveOutputBuffer(Request,
        sizeof(HID_DEVICE_ATTRIBUTES),
        &deviceAttributes,
        NULL);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "WdfRequestRetrieveOutputBuffer failed 0x%x\n", status);
        RegDebug(L"HidFx2GetDeviceAttributes WdfRequestRetrieveOutputBuffer failed", NULL, status);
        return status;
    }

    //
    // Retrieve USB device descriptor saved in device context
    //
    usbDeviceDescriptor = WdfMemoryGetBuffer(deviceInfo->DeviceDescriptor, NULL);

    deviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
    deviceAttributes->VendorID = usbDeviceDescriptor->idVendor;
    deviceAttributes->ProductID = usbDeviceDescriptor->idProduct;;
    deviceAttributes->VersionNumber = usbDeviceDescriptor->bcdDevice;

    RegDebug(L"HidFx2GetDeviceAttributes VendorID", NULL, usbDeviceDescriptor->idVendor);
    RegDebug(L"HidFx2GetDeviceAttributes ProductID", NULL, usbDeviceDescriptor->idProduct);
    RegDebug(L"HidFx2GetDeviceAttributes VersionNumber", NULL, usbDeviceDescriptor->bcdDevice);
    //
    // Report how many bytes were copied
    //
    WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL,
        "HidFx2GetDeviceAttributes Exit = 0x%x\n", status);

    RegDebug(L"HidFx2GetDeviceAttributes end", NULL, runtimes_ioctl_count);
    return status;
}


NTSTATUS
HidFx2SendIdleNotification(
    IN WDFREQUEST Request
)
/*++

Routine Description:

    Pass down Idle notification request to lower driver

Arguments:

    Request - Pointer to Request object.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                   status = STATUS_SUCCESS;
    BOOLEAN                    sendStatus = FALSE;
    WDF_REQUEST_SEND_OPTIONS   options;
    WDFIOTARGET                nextLowerDriver;
    WDFDEVICE                  device;
    PIO_STACK_LOCATION         currentIrpStack = NULL;
    IO_STACK_LOCATION          nextIrpStack;

    device = WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request));
    currentIrpStack = IoGetCurrentIrpStackLocation(WdfRequestWdmGetIrp(Request));

    //
    // Convert the request to corresponding USB Idle notification request
    //
    if (currentIrpStack->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO)) {

        status = STATUS_BUFFER_TOO_SMALL;
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "DeviceIoControl.InputBufferLength too small, 0x%x\n", status);
        RegDebug(L"HidFx2SendIdleNotification STATUS_BUFFER_TOO_SMALL", NULL, status);
        return status;
    }

    ASSERT(sizeof(HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO)
        == sizeof(USB_IDLE_CALLBACK_INFO));

#pragma warning(suppress :4127)  // conditional expression is constant warning
    if (sizeof(HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO) != sizeof(USB_IDLE_CALLBACK_INFO)) {

        status = STATUS_INFO_LENGTH_MISMATCH;
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "Incorrect DeviceIoControl.InputBufferLength, 0x%x\n", status);
        RegDebug(L"HidFx2SendIdleNotification STATUS_INFO_LENGTH_MISMATCH", NULL, status);
        return status;
    }

    //
    // prepare next stack location
    //
    RtlZeroMemory(&nextIrpStack, sizeof(IO_STACK_LOCATION));

    nextIrpStack.MajorFunction = currentIrpStack->MajorFunction;
    nextIrpStack.Parameters.DeviceIoControl.InputBufferLength =
        currentIrpStack->Parameters.DeviceIoControl.InputBufferLength;
    nextIrpStack.Parameters.DeviceIoControl.Type3InputBuffer =
        currentIrpStack->Parameters.DeviceIoControl.Type3InputBuffer;
    nextIrpStack.Parameters.DeviceIoControl.IoControlCode =
        IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION;
    nextIrpStack.DeviceObject =
        WdfIoTargetWdmGetTargetDeviceObject(WdfDeviceGetIoTarget(device));

    //
    // Format the I/O request for the driver's local I/O target by using the
    // contents of the specified WDM I/O stack location structure.
    //
    WdfRequestWdmFormatUsingStackLocation(
        Request,
        &nextIrpStack
    );

    //
    // Send the request down using Fire and forget option.
    //
    WDF_REQUEST_SEND_OPTIONS_INIT(
        &options,
        WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET
    );

    nextLowerDriver = WdfDeviceGetIoTarget(device);

    sendStatus = WdfRequestSend(
        Request,
        nextLowerDriver,
        &options
    );

    if (sendStatus == FALSE) {
        status = STATUS_UNSUCCESSFUL;
    }

    RegDebug(L"HidFx2SendIdleNotification end", NULL, runtimes_ioctl_count);
    return status;
}

PCHAR
DbgHidInternalIoctlString(
    IN ULONG        IoControlCode
)
/*++

Routine Description:

    Returns Ioctl string helpful for debugging

Arguments:

    IoControlCode - IO Control code

Return Value:

    Name String

--*/
{
    switch (IoControlCode)
    {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
    case IOCTL_HID_READ_REPORT:
        return "IOCTL_HID_READ_REPORT";
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
    case IOCTL_HID_WRITE_REPORT:
        return "IOCTL_HID_WRITE_REPORT";
    case IOCTL_HID_SET_FEATURE:
        return "IOCTL_HID_SET_FEATURE";
    case IOCTL_HID_GET_FEATURE:
        return "IOCTL_HID_GET_FEATURE";
    case IOCTL_HID_GET_STRING:
        return "IOCTL_HID_GET_STRING";
    case IOCTL_HID_ACTIVATE_DEVICE:
        return "IOCTL_HID_ACTIVATE_DEVICE";
    case IOCTL_HID_DEACTIVATE_DEVICE:
        return "IOCTL_HID_DEACTIVATE_DEVICE";
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
        return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
    default:
        return "Unknown IOCTL";
    }
}




NTSTATUS
PtpReportFeatures(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
)
{
    NTSTATUS Status;
    PDEVICE_EXTENSION pDevContext;
    PHID_XFER_PACKET pHidPacket;
    WDF_REQUEST_PARAMETERS RequestParameters;
    size_t ReportSize;

    PAGED_CODE();

    Status = STATUS_SUCCESS;
    pDevContext = GetDeviceContext(Device);

    WDF_REQUEST_PARAMETERS_INIT(&RequestParameters);
    WdfRequestGetParameters(Request, &RequestParameters);

    if (RequestParameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
    {
        RegDebug(L"STATUS_BUFFER_TOO_SMALL", NULL, 0x12345678);
        Status = STATUS_BUFFER_TOO_SMALL;
        goto exit;
    }

    pHidPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
    if (pHidPacket == NULL)
    {
        RegDebug(L"STATUS_INVALID_DEVICE_REQUEST", NULL, 0x12345678);
        Status = STATUS_INVALID_DEVICE_REQUEST;
        goto exit;
    }

    UCHAR reportId = pHidPacket->reportId;
    if (reportId == FAKE_REPORTID_DEVICE_CAPS) {//FAKE_REPORTID_DEVICE_CAPS//pDevContext->REPORTID_DEVICE_CAPS
        ReportSize = sizeof(PTP_DEVICE_CAPS_FEATURE_REPORT);
        if (pHidPacket->reportBufferLen < ReportSize) {
            Status = STATUS_INVALID_BUFFER_SIZE;
            RegDebug(L"PtpGetFeatures REPORTID_DEVICE_CAPS STATUS_INVALID_BUFFER_SIZE", NULL, pHidPacket->reportId);
            goto exit;
        }

        PPTP_DEVICE_CAPS_FEATURE_REPORT capsReport = (PPTP_DEVICE_CAPS_FEATURE_REPORT)pHidPacket->reportBuffer;

        capsReport->MaximumContactPoints = PTP_MAX_CONTACT_POINTS;// pDevContext->CONTACT_COUNT_MAXIMUM;// PTP_MAX_CONTACT_POINTS;
        capsReport->ButtonType = PTP_BUTTON_TYPE_CLICK_PAD;// pDevContext->PAD_TYPE;// PTP_BUTTON_TYPE_CLICK_PAD;
        capsReport->ReportID = FAKE_REPORTID_DEVICE_CAPS;// pDevContext->REPORTID_DEVICE_CAPS;//FAKE_REPORTID_DEVICE_CAPS
        RegDebug(L"PtpGetFeatures pHidPacket->reportId REPORTID_DEVICE_CAPS", NULL, pHidPacket->reportId);
        //RegDebug(L"PtpGetFeatures REPORTID_DEVICE_CAPS MaximumContactPoints", NULL, capsReport->MaximumContactPoints);
        //RegDebug(L"PtpGetFeatures REPORTID_DEVICE_CAPS REPORTID_DEVICE_CAPS ButtonType", NULL, capsReport->ButtonType);
    }
    else if (reportId == FAKE_REPORTID_PTPHQA) {//FAKE_REPORTID_PTPHQA//pDevContext->REPORTID_PTPHQA
            // Size sanity check
        ReportSize = sizeof(PTP_DEVICE_HQA_CERTIFICATION_REPORT);
        if (pHidPacket->reportBufferLen < ReportSize)
        {
            Status = STATUS_INVALID_BUFFER_SIZE;
            RegDebug(L"PtpGetFeatures REPORTID_PTPHQA STATUS_INVALID_BUFFER_SIZE", NULL, pHidPacket->reportId);
            goto exit;
        }

        PPTP_DEVICE_HQA_CERTIFICATION_REPORT certReport = (PPTP_DEVICE_HQA_CERTIFICATION_REPORT)pHidPacket->reportBuffer;

        *certReport->CertificationBlob = DEFAULT_PTP_HQA_BLOB;
        certReport->ReportID = FAKE_REPORTID_PTPHQA;//FAKE_REPORTID_PTPHQA//pDevContext->REPORTID_PTPHQA
        pDevContext->PtpInputModeOn = TRUE;//测试

        RegDebug(L"PtpGetFeatures pHidPacket->reportId REPORTID_PTPHQA", NULL, pHidPacket->reportId);

    }
    else {

        Status = STATUS_NOT_SUPPORTED;
        RegDebug(L"PtpGetFeatures pHidPacket->reportId STATUS_NOT_SUPPORTED", NULL, pHidPacket->reportId);
        goto exit;
    }

    WdfRequestSetInformation(Request, ReportSize);
    RegDebug(L"PtpGetFeatures STATUS_SUCCESS pDeviceContext->PtpInputOn", NULL, pDevContext->PtpInputModeOn);


exit:

    RegDebug(L"PtpGetFeatures end", NULL, runtimes_ioctl_count);
    return Status;
}


NTSTATUS
HidGetString(
    PDEVICE_EXTENSION pDevContext,
    WDFREQUEST Request
)
{
    RegDebug(L"HidGetString start", NULL, runtimes_ioctl_count);
    UNREFERENCED_PARAMETER(pDevContext);

    NTSTATUS status = STATUS_SUCCESS;

    PIRP pIrp = WdfRequestWdmGetIrp(Request);

    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(pIrp);//即是PIO_STACK_LOCATION IoStack = Irp->Tail.Overlay.CurrentStackLocation；


    USHORT stringSizeCb = 0;
    PWSTR string;

    //LONG dw = *(PULONG)IoStack->Parameters.DeviceIoControl.Type3InputBuffer;//注意这个Type3InputBuffer读取会蓝屏，所以需要测试得出实际wStrID调用顺序
    //USHORT wStrID = LOWORD(dw);//
    //RegDebug(L"HidGetString: wStrID=", NULL, wStrID);

    //switch (wStrID) {
    //case HID_STRING_ID_IMANUFACTURER:
    //    stringSizeCb = sizeof(MANUFACTURER_ID_STRING);
    //    string = MANUFACTURER_ID_STRING;
    //    break;
    //case HID_STRING_ID_IPRODUCT:
    //    stringSizeCb = sizeof(PRODUCT_ID_STRING);
    //    string = PRODUCT_ID_STRING;
    //    break;
    //case HID_STRING_ID_ISERIALNUMBER:
    //    stringSizeCb = sizeof(SERIAL_NUMBER_STRING);
    //    string = SERIAL_NUMBER_STRING;
    //    break;
    //default:
    //    status = STATUS_INVALID_PARAMETER;
    //    RegDebug(L"HidGetString: unkown string id", NULL, 0);
    //    goto exit;
    //}

    PUCHAR step = &pDevContext->GetStringStep;
    if (*step == 0) {
        *step = 1;
    }

    if (*step == 1) {// case HID_STRING_ID_IMANUFACTURER:
        (*step)++;
        stringSizeCb = sizeof(MANUFACTURER_ID_STRING);
        string = MANUFACTURER_ID_STRING;
        //RegDebug(L"HidGetString: HID_STRING_ID_IMANUFACTURER", string, stringSizeCb*2+2);
    }
    else if (*step == 2) {//case HID_STRING_ID_IPRODUCT:
        (*step)++;
        stringSizeCb = sizeof(PRODUCT_ID_STRING);
        string = PRODUCT_ID_STRING;
        //egDebug(L"HidGetString: HID_STRING_ID_IPRODUCT", string, stringSizeCb * 2 + 2);
    }
    else if (*step == 3) {//case HID_STRING_ID_ISERIALNUMBER:
        (*step)++;
        stringSizeCb = sizeof(SERIAL_NUMBER_STRING);
        string = SERIAL_NUMBER_STRING;
        //RegDebug(L"HidGetString: HID_STRING_ID_ISERIALNUMBER", string, stringSizeCb * 2 + 2);
    }
    else {
        status = STATUS_INVALID_PARAMETER;
        RegDebug(L"HidGetString: unkown string id", NULL, 0);
        goto exit;
    }


    ULONG bufferlength = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    //RegDebug(L"HidGetString: bufferlength=", NULL, bufferlength);
    int i = -1;
    do {
        ++i;
    } while (string[i]);

    stringSizeCb = (USHORT)(2 * i + 2);

    if (stringSizeCb > bufferlength)
    {
        status = STATUS_INVALID_BUFFER_SIZE;
        RegDebug(L"HidGetString STATUS_INVALID_BUFFER_SIZE", NULL, status);
        goto exit;
    }

    RtlMoveMemory(pIrp->UserBuffer, string, stringSizeCb);
    pIrp->IoStatus.Information = stringSizeCb;

exit:

    RegDebug(L"HidGetString end", NULL, runtimes_ioctl_count);
    return status;
}


VOID RegDebug(WCHAR* strValueName, PVOID dataValue, ULONG datasizeValue)//RegDebug(L"Run debug here",pBuffer,pBufferSize);//RegDebug(L"Run debug here",NULL,0x12345678);
{
    if (!debug_on) {//调试开关
        return;
    }

    //初始化注册表项
    UNICODE_STRING stringKey;
    RtlInitUnicodeString(&stringKey, L"\\Registry\\Machine\\Software\\RegDebug");

    //初始化OBJECT_ATTRIBUTES结构
    OBJECT_ATTRIBUTES  ObjectAttributes;
    InitializeObjectAttributes(&ObjectAttributes, &stringKey, OBJ_CASE_INSENSITIVE, NULL, NULL);//OBJ_CASE_INSENSITIVE对大小写敏感

    //创建注册表项
    HANDLE hKey;
    ULONG Des;
    NTSTATUS status = ZwCreateKey(&hKey, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL, REG_OPTION_NON_VOLATILE, &Des);
    if (NT_SUCCESS(status))
    {
        if (Des == REG_CREATED_NEW_KEY)
        {
            KdPrint(("新建注册表项！\n"));
        }
        else
        {
            KdPrint(("要创建的注册表项已经存在！\n"));
        }
    }
    else {
        return;
    }

    //初始化valueName
    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, strValueName);

    if (dataValue == NULL) {
        //设置REG_DWORD键值
        status = ZwSetValueKey(hKey, &valueName, 0, REG_DWORD, &datasizeValue, 4);
        if (!NT_SUCCESS(status))
        {
            KdPrint(("设置REG_DWORD键值失败！\n"));
        }
    }
    else {
        //设置REG_BINARY键值
        status = ZwSetValueKey(hKey, &valueName, 0, REG_BINARY, dataValue, datasizeValue);
        if (!NT_SUCCESS(status))
        {
            KdPrint(("设置REG_BINARY键值失败！\n"));
        }
    }
    ZwFlushKey(hKey);
    //关闭注册表句柄
    ZwClose(hKey);
}


VOID
Filter_DispatchPassThrough(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target
)
{
    //RegDebug(L"Filter_DispatchPassThrough", NULL, 0);

    WDF_REQUEST_SEND_OPTIONS options;
    BOOLEAN ret;
    NTSTATUS status = STATUS_SUCCESS;
    WDFDEVICE device = WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request));


    PDEVICE_EXTENSION pDevContext = GetDeviceContext(device);

    UNREFERENCED_PARAMETER(pDevContext);
    //
    // We are not interested in post processing the IRP so 
    // fire and forget.

    WDF_REQUEST_SEND_OPTIONS_INIT(&options,
        WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);//WDF_REQUEST_SEND_OPTION_TIMEOUT  //WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET

    //将I/O请求发送到下层设备前，做相应的数据处理//不设置完成例程时，下面这句可有可无
    //WdfRequestFormatRequestUsingCurrentType(Request);


    //将 I/O 请求发送到下层设备
    ret = WdfRequestSend(Request, Target, &options);//WDF_NO_SEND_OPTIONS
    if (ret == FALSE) {
        status = WdfRequestGetStatus(Request);
        RegDebug(L"WdfRequestSend failed", NULL, status);
        WdfRequestComplete(Request, status);
    }

    RegDebug(L"Filter_DispatchPassThrough WdfRequestSend ok", 0, status);

    return;
}


NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
/*++

Routine Description:

    Installable driver initialization entry point.
    This entry point is called directly by the I/O system.

Arguments:

    DriverObject - pointer to the driver object

    RegistryPath - pointer to a unicode string representing the path,
                   to driver-specific key in the registry.

Return Value:

    STATUS_SUCCESS if successful,
    STATUS_UNSUCCESSFUL otherwise.

--*/
{
    NTSTATUS               status = STATUS_SUCCESS;
    WDF_DRIVER_CONFIG      config;
    WDF_OBJECT_ATTRIBUTES  attributes;

    //
    // Initialize WPP Tracing
    //
    WPP_INIT_TRACING(DriverObject, RegistryPath);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
        "HIDUSBFX2 Driver Sample\n");

    RegDebug(L"DriverEntry start", NULL, status);

    WDF_DRIVER_CONFIG_INIT(&config, HidFx2EvtDeviceAdd);

    //
    // Register a cleanup callback so that we can call WPP_CLEANUP when
    // the framework driver object is deleted during driver unload.
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = HidFx2EvtDriverContextCleanup;

    //
    // Create a framework driver object to represent our driver.
    //
    status = WdfDriverCreate(DriverObject,
        RegistryPath,
        &attributes,      // Driver Attributes
        &config,          // Driver Config Info
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfDriverCreate failed with status 0x%x\n", status);

        WPP_CLEANUP(DriverObject);
    }

    RegDebug(L"DriverEntry ok", NULL, status);
    return status;
}


NTSTATUS
HidFx2EvtDeviceAdd(
    IN WDFDRIVER       Driver,
    IN PWDFDEVICE_INIT DeviceInit
)
/*++
Routine Description:

    HidFx2EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a WDF device object to
    represent a new instance of toaster device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                      status = STATUS_SUCCESS;
    WDF_IO_QUEUE_CONFIG           queueConfig;
    WDF_OBJECT_ATTRIBUTES         attributes;
    WDFDEVICE                     hDevice;
    PDEVICE_EXTENSION             devContext = NULL;
    WDFQUEUE                      queue;
    WDF_PNPPOWER_EVENT_CALLBACKS  pnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
        "HidFx2EvtDeviceAdd called\n");

    //
    // Tell framework this is a filter driver. Filter drivers by default are
    // not power policy owners. This works well for this driver because
    // HIDclass driver is the power policy owner for HID minidrivers.
    //
    WdfFdoInitSetFilter(DeviceInit);

    //
    // Initialize pnp-power callbacks, attributes and a context area for the device object.
    //
    //
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

    //
    // For usb devices, PrepareHardware callback is the to place select the
    // interface and configure the device.
    //
    pnpPowerCallbacks.EvtDevicePrepareHardware = HidFx2EvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = ReleaseHardware;

    //
    // These two callbacks start and stop the wdfusb pipe continuous reader
    // as we go in and out of the D0-working state.
    //
    pnpPowerCallbacks.EvtDeviceD0Entry = HidFx2EvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = HidFx2EvtDeviceD0Exit;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_EXTENSION);

    //
    // Create a framework device object.This call will in turn create
    // a WDM device object, attach to the lower stack, and set the
    // appropriate flags and attributes.
    //
    status = WdfDeviceCreate(&DeviceInit, &attributes, &hDevice);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfDeviceCreate failed with status code 0x%x\n", status);
        RegDebug(L"HidFx2EvtDeviceAdd WdfDeviceCreate failed", NULL, status);
        return status;
    }


    devContext = GetDeviceContext(hDevice);
    devContext->fxDevice = hDevice;
    devContext->IoTarget= WdfDeviceGetIoTarget(hDevice);
    devContext->DeviceObject = WdfIoTargetWdmGetTargetDeviceObject(devContext->IoTarget);
    RegDebug(L"devContext->DeviceObject=", devContext->DeviceObject->DriverObject->DriverName.Buffer, devContext->DeviceObject->DriverObject->DriverName.Length);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = HidFx2EvtInternalDeviceControl;

    status = WdfIoQueueCreate(hDevice,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue
    );
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfIoQueueCreate failed 0x%x\n", status);
        RegDebug(L"HidFx2EvtDeviceAdd WdfIoQueueCreate failed", NULL, status);
        return status;
    }

    //
    // Register a manual I/O queue for handling Interrupt Message Read Requests.
    // This queue will be used for storing Requests that need to wait for an
    // interrupt to occur before they can be completed.
    //
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

    //
    // This queue is used for requests that dont directly access the device. The
    // requests in this queue are serviced only when the device is in a fully
    // powered state and sends an interrupt. So we can use a non-power managed
    // queue to park the requests since we dont care whether the device is idle
    // or fully powered up.
    //
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(hDevice,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &devContext->InterruptMsgQueue
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfIoQueueCreate failed 0x%x\n", status);
        RegDebug(L"HidFx2EvtDeviceAdd WdfIoQueueCreate InterruptMsgQueue failed", NULL, status);
        return status;
    }


    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = hDevice;
    status = WdfMemoryCreate(
        &attributes,
        NonPagedPoolNx,
        0,
        1024,//buffsize
        &devContext->ptpRequestMemory,
        &devContext->ptpRequestBufferPointer
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfMemoryCreate for Device Descriptor failed %!STATUS!\n",
            status);

        RegDebug(L"HidFx2EvtDeviceAdd WdfMemoryCreate ptpRequestMemory failed", NULL, status);
        return status;
    }

    //
   // Create a timer to handle debouncing of switchpack
   //
    WDF_TIMER_CONFIG              timerConfig;
    WDFTIMER                      timerHandle;

    WDF_TIMER_CONFIG_INIT(
        &timerConfig,
        HidFx2EvtTimerFunction
    );
    timerConfig.AutomaticSerialization = FALSE;
    timerConfig.UseHighResolutionTimer = TRUE;    //UseHighResolutionTimer 高解析度计时器

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = hDevice;
    status = WdfTimerCreate(
        &timerConfig,
        &attributes,
        &timerHandle
    );
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfTimerCreate failed status:0x%x\n", status);
        return status;
    }

    devContext->DebounceTimer = timerHandle;

    return status;
}


VOID
HidFx2EvtDriverContextCleanup(
    IN WDFOBJECT Object
)
/*++
Routine Description:

    Free resources allocated in DriverEntry that are not automatically
    cleaned up framework.

Arguments:

    Driver - handle to a WDF Driver object.

Return Value:

    VOID.

--*/
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Object);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Exit HidFx2EvtDriverContextCleanup\n");

    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)Object));
}


#if !defined(EVENT_TRACING)

VOID
TraceEvents(
    IN ULONG   TraceEventsLevel,
    IN ULONG   TraceEventsFlag,
    IN PCCHAR  DebugMessage,
    ...
)

/*++

Routine Description:

    Debug print for the sample driver.

Arguments:

    TraceEventsLevel - print level between 0 and 3, with 3 the most verbose

Return Value:

    None.

 --*/
{
#if DBG
#define     TEMP_BUFFER_SIZE        512
    va_list    list;
    CHAR       debugMessageBuffer[TEMP_BUFFER_SIZE];
    NTSTATUS   status;

    va_start(list, DebugMessage);

    if (DebugMessage) {

        //
        // Using new safe string functions instead of _vsnprintf.
        // This function takes care of NULL terminating if the message
        // is longer than the buffer.
        //
        status = RtlStringCbVPrintfA(debugMessageBuffer,
            sizeof(debugMessageBuffer),
            DebugMessage,
            list);
        if (!NT_SUCCESS(status)) {

            DbgPrint(_DRIVER_NAME_": RtlStringCbVPrintfA failed 0x%x\n", status);
            return;
        }
        if (TraceEventsLevel <= TRACE_LEVEL_ERROR ||
            (TraceEventsLevel <= DebugLevel &&
                ((TraceEventsFlag & DebugFlag) == TraceEventsFlag))) {
            DbgPrint("%s%s", _DRIVER_NAME_, debugMessageBuffer);
        }
    }
    va_end(list);

    return;
#else
    UNREFERENCED_PARAMETER(TraceEventsLevel);
    UNREFERENCED_PARAMETER(TraceEventsFlag);
    UNREFERENCED_PARAMETER(DebugMessage);
#endif
}

#endif



_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(
    _In_ WDFDEVICE Device
)
{
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    NTSTATUS                            status = STATUS_SUCCESS;
    PDEVICE_EXTENSION                     pDeviceContext;
    WDFUSBPIPE                          pipe;
    WDF_USB_PIPE_INFORMATION            pipeInfo;
    UCHAR                               index;
    UCHAR                               numberConfiguredPipes;

    PAGED_CODE();

    pDeviceContext = GetDeviceContext(Device);
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);

    // It is slightly different than UMDF
    status = WdfUsbTargetDeviceSelectConfig(pDeviceContext->UsbDevice,
        WDF_NO_OBJECT_ATTRIBUTES,
        &configParams);

    if (!NT_SUCCESS(status)) {
        RegDebug(L"SelectInterruptInterface WdfUsbTargetDeviceSelectConfig failed", NULL, status);
        return status;
    }

    pDeviceContext->UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;
    numberConfiguredPipes = configParams.Types.SingleInterface.NumberConfiguredPipes;
    pDeviceContext->NumberConfiguredPipes = numberConfiguredPipes;
    RegDebug(L"SelectInterruptInterface NumberConfiguredPipes", NULL, numberConfiguredPipes);
    //
    // Get pipe handles
    //
    for (index = 0; index < numberConfiguredPipes; index++) {

        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

        pipe = WdfUsbInterfaceGetConfiguredPipe(
            pDeviceContext->UsbInterface,
            index, //PipeIndex,
            &pipeInfo
        );

        //
        // Tell the framework that it's okay to read less than
        // MaximumPacketSize
        //
        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);
        RegDebug(L"SelectInterruptInterface pipeInfo.PipeType", NULL, pipeInfo.PipeType);

        if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType) {
            pDeviceContext->InterruptPipe = pipe;
            pDeviceContext->pipeMaxPacketSize = pipeInfo.MaximumPacketSize;
            /*RegDebug(L"SelectInterruptInterface pipeMaxPacketSize=", NULL, pDeviceContext->pipeMaxPacketSize);
            RegDebug(L"SelectInterruptInterface pipeInfo.MaximumTransferSize=", NULL, pipeInfo.MaximumTransferSize);
            RegDebug(L"SelectInterruptInterface pipeInfo.EndpointAddress=", NULL, pipeInfo.EndpointAddress);
            RegDebug(L"SelectInterruptInterface pipeInfo.Interval=", NULL, pipeInfo.Interval);
            RegDebug(L"SelectInterruptInterface pipeInfo.SettingIndex=", NULL, pipeInfo.SettingIndex);*/
            break;
        }

    }

    //
    // If we didn't find interrupt pipe, fail the start.
    //
    if (!pDeviceContext->InterruptPipe) {
        status = STATUS_INVALID_DEVICE_STATE;
        RegDebug(L"SelectInterruptInterface STATUS_INVALID_DEVICE_STATE", NULL, status);
        return status;
    }

    RegDebug(L"SelectInterruptInterface end", NULL, status);
    return status;
}



BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE Pipe,
    _In_ NTSTATUS Status,
    _In_ USBD_STATUS UsbdStatus
)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(UsbdStatus);
    UNREFERENCED_PARAMETER(Status);

    RegDebug(L"AmtPtpEvtUsbInterruptReadersFailed end", NULL, runtimes_ioRead);
    return TRUE;
}

//////
//NTSTATUS CallUSB(PDEVICE_EXTENSION pDeviceContext)
//{
//    NTSTATUS status = STATUS_SUCCESS;
//    KEVENT event;
//    IO_STATUS_BLOCK IoStatusBlock = { 0 };
//
//    KeInitializeEvent(&event, SynchronizationEvent, FALSE);
//    PIRP irp = IoAllocateIrp(WdfDeviceWdmGetDeviceObject(pDeviceContext->fxDevice)->StackSize + 2, FALSE);
//    if (!irp) {
//        return STATUS_NO_MEMORY;
//    }
//
//    /// fill data
//    static CHAR buffer[256]; ////
//    HID_XFER_PACKET* hxp = (HID_XFER_PACKET*)buffer;
//    hxp->reportBuffer = (PUCHAR)buffer + sizeof(HID_XFER_PACKET);
//    hxp->reportBufferLen = 2;
//    hxp->reportId = REPORTID_MOUSE;
//
//    /////
//    PDEVICE_OBJECT DeviceObject = pDeviceContext->DeviceObject;// WdfIoTargetWdmGetTargetDeviceObject(pDeviceContext->IoTarget);
//    /////
//    irp->AssociatedIrp.SystemBuffer = buffer;
//    irp->UserBuffer = buffer;
//    irp->UserEvent = &event;
//    irp->UserIosb = &IoStatusBlock;
//    irp->Tail.Overlay.Thread = PsGetCurrentThread();
//    irp->Tail.Overlay.OriginalFileObject = NULL;
//    irp->RequestorMode = KernelMode;
//    irp->Flags = 0;
//
//    /////
//    PIO_STACK_LOCATION irpStack = IoGetNextIrpStackLocation(irp);
//    irpStack->DeviceObject = DeviceObject;
//    irpStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
//
//    irpStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_HID_READ_REPORT;
//    irpStack->Parameters.DeviceIoControl.InputBufferLength = sizeof(HID_XFER_PACKET);
//
//    ////
//    IoSetCompletionRoutine(irp, CallUSBComplete, 0, TRUE, TRUE, TRUE);
//
//    status = IoCallDriver(DeviceObject, irp);
//    if (status == STATUS_PENDING) {
//        //
//        KeWaitForSingleObject(&event, Executive, KernelMode, TRUE, 0);
//
//        status = IoStatusBlock.Status;
//    }
//
//    //////
//    return status;
//}


NTSTATUS 
StartAllPipes(
    _In_ PDEVICE_EXTENSION DeviceContext
)
{
    NTSTATUS status= STATUS_SUCCESS;
    UCHAR count, i;
    
    count = DeviceContext->NumberConfiguredPipes;
    for (i = 0; i < count; i++) {
        WDFUSBPIPE pipe;
        WDF_USB_PIPE_INFORMATION            pipeInfo;
        pipe = WdfUsbInterfaceGetConfiguredPipe(DeviceContext->UsbInterface,
            i, //PipeIndex,
            &pipeInfo
        );

        //WdfUsbTargetPipeGetInformation(pipe, &pipeInfo);
        RegDebug(L"StartAllPipes pipeMaxPacketSize=", NULL, pipeInfo.MaximumPacketSize);
        RegDebug(L"StartAllPipes pipeInfo.MaximumTransferSize=", NULL, pipeInfo.MaximumTransferSize);
        RegDebug(L"StartAllPipes pipeInfo.EndpointAddress=", NULL, pipeInfo.EndpointAddress);
        RegDebug(L"StartAllPipes pipeInfo.Interval=", NULL, pipeInfo.Interval);
        RegDebug(L"SelectInterruptInterface pipeInfo.SettingIndex=", NULL, pipeInfo.SettingIndex);
        RegDebug(L"StartAllPipes pipeInfo.PipeType=", NULL, pipeInfo.PipeType);
        //status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(pipe));
        //if (!NT_SUCCESS(status)) {
        //    RegDebug(L"StartAllPipes failed pipe=", NULL, i);
        //    return STATUS_UNSUCCESSFUL;
        //}
    }

    RegDebug(L"StartAllPipes end", NULL, status);
    return status;
}


NTSTATUS
StopAllPipes(
    _In_ PDEVICE_EXTENSION DeviceContext
)
{
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR count, i;

    count = DeviceContext->NumberConfiguredPipes;
    for (i = 0; i < count; i++) {
        WDFUSBPIPE pipe;
        pipe = WdfUsbInterfaceGetConfiguredPipe(DeviceContext->UsbInterface,
            i, //PipeIndex,
            NULL
        );
        WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(pipe),
            WdfIoTargetCancelSentIo);
    }

    RegDebug(L"StopAllPipes end", NULL, status);
    return status;
}


VOID
HidFx2EvtTimerFunction(
    IN WDFTIMER  Timer
)
{
    PDEVICE_EXTENSION  devContext =  GetDeviceContext(WdfTimerGetParentObject(Timer));
    
    runtimes_timer++;
    NTSTATUS     Status = STATUS_SUCCESS;

    //RegDebug(L"HidFx2EvtTimerFunction start", NULL, runtimes_timer);
    KdPrint(("HidFx2EvtTimerFunction start start %x", runtimes_timer));

    WDFREQUEST Request;

    // Retrieve next PTP touchpad request.
    Status = WdfIoQueueRetrieveNextRequest(
        devContext->InterruptMsgQueue,
        &Request
    );

    if (!NT_SUCCESS(Status)) {
        //RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete WdfIoQueueRetrieveNextRequest err", NULL, Status);
        return;
    }

    Status = WdfRequestRetrieveOutputMemory(
        Request,
        &devContext->ptpRequestMemory
    );

    if (!NT_SUCCESS(Status)) {
        //RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete WdfRequestRetrieveOutputMemory err", NULL, Status);
        return;
    }

    // Prepare report
    //PTP_REPORT PtpReport;

    struct mouse_report_t mreport;
    RtlZeroMemory(&mreport, sizeof(mreport));

    //if (NumBytesTransferred >= sizeof(PTP_REPORT)) {

    //    RtlCopyBytes(&PtpReport, TouchBuffer, sizeof(PTP_REPORT));//
    //}

    mreport.dx = 1;
    mreport.dy = 1;
    mreport.report_id = FAKE_REPORTID_MOUSE;

    // Compose final report and write it back
    Status = WdfMemoryCopyFromBuffer(
        devContext->ptpRequestMemory,
        0,
        (PVOID)&mreport,
        sizeof(mreport)
    );

    if (!NT_SUCCESS(Status)) {
        //RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete WdfMemoryCopyFromBuffer err", NULL, Status);
        return;
    }

    // Set result
    WdfRequestSetInformation(Request, sizeof(mreport));//NumBytesTransferred//sizeof(PTP_REPORT)

    // Set completion flag
    WdfRequestComplete(Request, Status);

        //RegDebug(L"HidFx2EvtTimerFunction WdfRequestComplete", NULL, runtimes_ioRead);

}



VOID
DispatchRead(
    IN PDEVICE_EXTENSION devContext
)
{
    if (!devContext->bReadCompletion) {
        KdPrint(("DispatchRead no data"));
        return;
    }

    //RegDebug(L"DispatchRead start", NULL, runtimes_ioRead);
    KdPrint(("DispatchRead start %x", runtimes_ioRead));
    NTSTATUS Status = STATUS_SUCCESS;
    WDFREQUEST ptpRequest;


    struct mouse_report_t mreport;
    size_t bytesToCopy = 0;
    size_t bytesReturned = 0;

    // Retrieve next PTP touchpad request.
    Status = WdfIoQueueRetrieveNextRequest(
        devContext->InterruptMsgQueue,
        &ptpRequest
    );

    if (!NT_SUCCESS(Status)) {
        KdPrint(("DispatchRead WdfIoQueueRetrieveNextRequest err %x", Status));
        //RegDebug(L"DispatchRead WdfIoQueueRetrieveNextRequest err", NULL, Status);
        return;
    }

    Status = WdfRequestRetrieveOutputBuffer(ptpRequest,
        bytesToCopy,
        (PVOID*)&mreport,
        &bytesReturned);// BufferLength

    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
            "WdfRequestRetrieveOutputBuffer failed with status: 0x%x\n", Status);
        KdPrint(("DispatchRead WdfRequestRetrieveOutputBuffer err %x", Status));
        //RegDebug(L"DispatchRead WdfRequestRetrieveOutputBuffer err", NULL, Status);
        return;
    }

    bytesToCopy = sizeof(struct mouse_report_t);
    bytesReturned = sizeof(struct mouse_report_t);


    // Prepare report
    //PTP_REPORT PtpReport;
    RtlZeroMemory(&mreport, sizeof(mreport));

    //if (NumBytesTransferred >= sizeof(PTP_REPORT)) {

    //    RtlCopyBytes(&PtpReport, TouchBuffer, sizeof(PTP_REPORT));//
    //}

    mreport.button = devContext->TouchData[1];
    mreport.dx = devContext->TouchData[2]; //1;
    mreport.dy = devContext->TouchData[3]; //1;
    mreport.v_wheel = devContext->TouchData[4];
    mreport.report_id = FAKE_REPORTID_MOUSE;

    KdPrint(("DispatchRead TouchData.dx= %x, dy= %x\n", devContext->TouchData[2], devContext->TouchData[3]));
    KdPrint(("DispatchRead mreport.dx= %x, dy= %x\n", mreport.dx, mreport.dy));
    //RegDebug(L"DispatchRead mreport=", &mreport, 5);

    WdfRequestCompleteWithInformation(ptpRequest, Status, bytesReturned);//NumBytesTransferred

    //// Set result
    //WdfRequestSetInformation(ptpRequest, sizeof(struct mouse_report_t));//NumBytesTransferred//sizeof(PTP_REPORT)

    //// Set completion flag
    //WdfRequestComplete(ptpRequest, Status);

    devContext->bReadCompletion = FALSE;

    KdPrint(("DispatchRead end %x", runtimes_ioRead));
    //RegDebug(L"DispatchRead end", NULL, runtimes_ioRead);
}


VOID
DispatchReadRequest(
    IN PDEVICE_EXTENSION devContext
)
{

    KdPrint(("DispatchReadRequest start%x", runtimes_ioRead));
    //RegDebug(L"DispatchReadRequest start", NULL, runtimes_ioRead);

    if (!devContext->bReadCompletion) {
        KdPrint(("DispatchRead no data"));
        return;
    }

    //RegDebug(L"DispatchRead start", NULL, runtimes_ioRead);
    KdPrint(("DispatchRead start %x", runtimes_ioRead));
    NTSTATUS Status = STATUS_SUCCESS;
    WDFREQUEST ptpRequest;

    // Retrieve next PTP touchpad request.
    Status = WdfIoQueueRetrieveNextRequest(
        devContext->InterruptMsgQueue,
        &ptpRequest
    );

    if (!NT_SUCCESS(Status)) {
        KdPrint(("DispatchReadRequest WdfIoQueueRetrieveNextRequest err %x", Status));
        //RegDebug(L"DispatchReadRequest WdfIoQueueRetrieveNextRequest err", NULL, Status);
        return;
    }

    Status = WdfRequestRetrieveOutputMemory(
        ptpRequest,
        &devContext->ptpRequestMemory
    );

    if (!NT_SUCCESS(Status)) {
        KdPrint(("DispatchReadRequest WdfRequestRetrieveOutputMemory err %x", runtimes_ioRead));
        //RegDebug(L"DispatchReadRequest WdfRequestRetrieveOutputMemory err", NULL, runtimes_ioRead);
        return;
    }

    // Prepare report
    //PTP_REPORT PtpReport;

    struct mouse_report_t mreport;
    RtlZeroMemory(&mreport, sizeof(mreport));

    //if (NumBytesTransferred >= sizeof(PTP_REPORT)) {

    //    RtlCopyBytes(&PtpReport, TouchBuffer, sizeof(PTP_REPORT));//
    //}

    mreport.button = devContext->TouchData[1];
    mreport.dx = 1;// devContext->TouchData[2]; //1;
    mreport.dy = 1;// devContext->TouchData[3]; //1;
    mreport.v_wheel = devContext->TouchData[4];
    mreport.report_id = FAKE_REPORTID_MOUSE;

    KdPrint(("DispatchReadRequest TouchData.dx= %x, dy= %x\n", devContext->TouchData[2], devContext->TouchData[3]));
    KdPrint(("DispatchReadRequest mreport.dx= %x, dy= %x\n", mreport.dx, mreport.dy));
    RegDebug(L"DispatchReadRequest mreport=", &mreport, 5);

    // Compose final report and write it back
    Status = WdfMemoryCopyFromBuffer(
        devContext->ptpRequestMemory,
        0,
        (PVOID)&mreport,
        sizeof(struct mouse_report_t)
    );

    if (!NT_SUCCESS(Status)) {
        //RegDebug(L"DispatchReadRequest WdfMemoryCopyFromBuffer err", NULL, Status);
        KdPrint(("DispatchReadRequest WdfMemoryCopyFromBuffer err %x", Status));
        return;
    }

    // Set result
    WdfRequestSetInformation(ptpRequest, sizeof(struct mouse_report_t));//NumBytesTransferred//sizeof(PTP_REPORT)

    // Set completion flag
    WdfRequestComplete(ptpRequest, Status);

    devContext->bReadCompletion = FALSE;

    KdPrint(("DispatchReadRequest end %x", runtimes_ioRead));
    //RegDebug(L"DispatchReadRequest end", NULL, runtimes_ioRead);

}
