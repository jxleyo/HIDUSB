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
HidFx2EvtDevicePrepareHardware(IN WDFDEVICE Device,IN WDFCMRESLIST ResourceList,IN WDFCMRESLIST ResourceListTranslated)
{
    NTSTATUS                            status = STATUS_SUCCESS;
    PDEVICE_EXTENSION                   devContext = NULL;
    WDF_OBJECT_ATTRIBUTES               attributes;
    PUSB_DEVICE_DESCRIPTOR              usbDeviceDescriptor = NULL;

    WDF_USB_DEVICE_INFORMATION deviceInfo;
    ULONG waitWakeEnable = FALSE;

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    PAGED_CODE ();

    RegDebug(L"HidFx2EvtDevicePrepareHardware start", NULL, status);

    devContext = GetDeviceContext(Device);
    if (devContext->UsbDevice == NULL) {
        status = WdfUsbTargetDeviceCreate(Device,
                                  WDF_NO_OBJECT_ATTRIBUTES,
                                  &devContext->UsbDevice);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                "WdfUsbTargetDeviceCreate failed 0x%x\n", status);

            RegDebug(L"HidFx2EvtDevicePrepareHardware WdfUsbTargetDeviceCreate failed", NULL, status);
            return status;
        }
    }

    devContext->UsbIoTarget = WdfUsbTargetDeviceGetIoTarget(devContext->UsbDevice);
    if (!devContext->UsbIoTarget) {
        RegDebug(L"HidFx2EvtDevicePrepareHardware WdfUsbTargetDeviceGetIoTarget UsbIoTarget failed", NULL, status);
    }

    //
    WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);

    status = WdfUsbTargetDeviceRetrieveInformation(
        devContext->UsbDevice,
        &deviceInfo);
    if (NT_SUCCESS(status)) {
        //RegDebug(L"EvtDevicePrepareHardware WdfUsbTargetDeviceRetrieveInformation start", NULL, status);
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "IsDeviceHighSpeed: %s\n",
            (deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED) ? "TRUE" : "FALSE");
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
            "IsDeviceSelfPowered: %s\n",
            (deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_SELF_POWERED) ? "TRUE" : "FALSE");

        waitWakeEnable = deviceInfo.Traits &
            WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE;

        devContext->UsbDeviceTraits = deviceInfo.Traits;
        RegDebug(L"HIDUSB_KMDF_EvtDevicePrepareHardware deviceInfo.Traits=", NULL, deviceInfo.Traits);
        RegDebug(L"HIDUSB_KMDF_EvtDevicePrepareHardware waitWakeEnable=", NULL, waitWakeEnable);
        RegDebug(L"HIDUSB_KMDF_EvtDevicePrepareHardware deviceInfo.UsbdVersionInformation=", NULL, deviceInfo.UsbdVersionInformation.USBDI_Version);

    }
    else {
        RegDebug(L"HIDUSB_KMDF_EvtDevicePrepareHardware WdfUsbTargetDeviceRetrieveInformation failed", NULL, status);
        devContext->UsbDeviceTraits = 0;
    }

    // Get the device descriptor and store it in device context
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
        RegDebug(L"HidFx2EvtDevicePrepareHardware WdfMemoryCreate failed", NULL, status);
        return status;
    }

    WdfUsbTargetDeviceGetDeviceDescriptor(
        devContext->UsbDevice,
        usbDeviceDescriptor
    );

    RegDebug(L"HidFx2EvtDevicePrepareHardware WdfUsbTargetDeviceGetDeviceDescriptor=", usbDeviceDescriptor, sizeof(USB_DEVICE_DESCRIPTOR));//

    // Select interface to use
    status = SelectInterruptInterface(Device);
    if (!NT_SUCCESS(status)) {
        RegDebug(L"HidFx2EvtDevicePrepareHardware SelectInterruptInterface failed", NULL, status);
        return status;
    }

    //configure continuous reader
    status = HidFx2ConfigContReaderForInterruptEndPoint(devContext);

    RegDebug(L"HidFx2EvtDevicePrepareHardware ok", NULL, status);
    return status;
}


NTSTATUS
HidFx2ConfigContReaderForInterruptEndPoint(PDEVICE_EXTENSION DeviceContext)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE ();
    //RegDebug(L"HidFx2ConfigContReaderForInterruptEndPoint start", NULL, status);

    size_t TransferLength = 0;
    TransferLength = DeviceContext->pipeMaxPacketSize;// sizeof(PTP_REPORT);

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&contReaderConfig,
                                          HidFx2EvtUsbInterruptPipeReadComplete,
                                          DeviceContext,    // Context
                                          TransferLength);   // TransferLength//sizeof(UCHAR))

    contReaderConfig.EvtUsbTargetPipeReadersFailed = HidFx2EvtUsbInterruptReadersFailed;

    status = WdfUsbTargetPipeConfigContinuousReader(DeviceContext->InterruptPipe, &contReaderConfig);
    if (!NT_SUCCESS(status)) {
        RegDebug(L"HidFx2ConfigContReaderForInterruptEndPoint WdfUsbTargetPipeConfigContinuousReader failed", NULL, status);
        return status;
    }

    RegDebug(L"HidFx2ConfigContReaderForInterruptEndPoint end", NULL, status);
    return status;
}


VOID
HidFx2EvtUsbInterruptPipeReadComplete(WDFUSBPIPE Pipe, WDFMEMORY Buffer, size_t NumBytesTransferred, WDFCONTEXT  Context)
{
    PDEVICE_EXTENSION  devContext = Context;

    UNREFERENCED_PARAMETER(devContext);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(NumBytesTransferred);
    UNREFERENCED_PARAMETER(Pipe);

    if (!ioReady) {//接触触控板前会执行一次该事件
        return;
    }

    runtimes_ioRead++;//经过测试会有读取完成
    //RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete start", NULL, runtimes_ioctl_count);//RegDebug会导致蓝屏

    if (NumBytesTransferred == 0 || NumBytesTransferred < 5) {//测试结果恒定为5,数据为鼠标集合，或者为10触摸板集合//sizeof(PTP_REPORT)
        KdPrint(("HidFx2EvtUsbInterruptPipeReadComplete NumBytesTransferred err %x", runtimes_ioRead));
        //RegDebug(L"HidFx2EvtUsbInterruptPipeReadComplete NumBytesTransferred err", NULL, runtimes_ioRead);
        return;
    }

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

    RtlCopyMemory(devContext->TouchData, TouchBuffer, NumBytesTransferred);

    KdPrint(("HidFx2EvtUsbInterruptPipeReadComplete TouchBuffer= 0x%x,0x%x,0x%x,0x%x,0x%x\n", \
        devContext->TouchData[0], devContext->TouchData[1], devContext->TouchData[2], devContext->TouchData[3], devContext->TouchData[4]));

    //DispatchReadRequest(devContext, (ULONG)NumBytesTransferred);

    KdPrint(("HidFx2EvtUsbInterruptPipeReadComplete end %x", runtimes_ioRead));
}



NTSTATUS
HidFx2EvtDeviceD0Entry(IN  WDFDEVICE Device, IN  WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    PDEVICE_EXTENSION   devContext = NULL;
    NTSTATUS            status = STATUS_SUCCESS;

    devContext = GetDeviceContext(Device);
    RegDebug(L"HidFx2EvtDeviceD0Entry start", NULL, status);

    devContext->isTargetStarted = FALSE;

    devContext->PtpInputModeOn = FALSE;
    devContext->GetStringStep = 0;

    ioReady = FALSE;
    runtimes_ioctl_count = 0;
    runtimes_ioRead = 0;
    runtimes_timer = 0;

    RtlZeroMemory(&devContext->TouchData, 30);//

    WDFUSBPIPE pipe = devContext->InterruptPipe;
    WDF_USB_PIPE_INFORMATION            pipeInfo;

    WdfUsbTargetPipeGetInformation(pipe, &pipeInfo);

    RegDebug(L"HidFx2EvtDeviceD0Entry check pipeMaxPacketSize=", NULL, pipeInfo.MaximumPacketSize);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.MaximumTransferSize=", NULL, pipeInfo.MaximumTransferSize);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.EndpointAddress=", NULL, pipeInfo.EndpointAddress);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.Interval=", NULL, pipeInfo.Interval);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.SettingIndex=", NULL, pipeInfo.SettingIndex);
    //RegDebug(L"HidFx2EvtDeviceD0Entry check pipeInfo.PipeType=", NULL, pipeInfo.PipeType);

    //status = StartAllPipes(devContext); 

    // Start the target. This will start the continuous reader
    WDFIOTARGET UsbIoTarget = WdfUsbTargetPipeGetIoTarget(devContext->InterruptPipe);
    if (UsbIoTarget) {
        status = WdfIoTargetStart(UsbIoTarget);//WdfUsbTargetPipeGetIoTarget(devContext->InterruptPipe);
        if (NT_SUCCESS(status)) {
            devContext->isTargetStarted = TRUE;
            RegDebug(L"HidFx2EvtDeviceD0Entry WdfIoTargetStart ok", NULL, status);
        }
        else {
            if (devContext->isTargetStarted) {
                WdfIoTargetStop(UsbIoTarget, WdfIoTargetCancelSentIo);
            }
        }
    }

    RegDebug(L"HidFx2EvtDeviceD0Entry end", NULL, status);
    return status;
}


NTSTATUS
HidFx2EvtDeviceD0Exit(IN  WDFDEVICE Device,IN  WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_EXTENSION         devContext;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(TargetState);

    devContext = GetDeviceContext(Device);
    NTSTATUS status = STATUS_SUCCESS;

    WDFIOTARGET UsbTarget = WdfUsbTargetPipeGetIoTarget(devContext->InterruptPipe);
    if (UsbTarget) {
        WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(// 如果还有未完成的IO操作，都Cancel掉。
            devContext->InterruptPipe), WdfIoTargetCancelSentIo);

        //status = WdfUsbTargetPipeAbortSynchronously(
        //    devContext->InterruptPipe,
        //    WDF_NO_HANDLE,
        //    NULL
        //);

        devContext->isTargetStarted = FALSE;
        RegDebug(L"HidFx2EvtDeviceD0Exit WdfIoTargetStop ok", NULL, runtimes_ioRead);
    }

    status= StopAllPipes(devContext);

    // 完成在手动队列中的所有未完成Request。
    // 如果Queue处于未启动状态，会返回STATUS_WDF_PAUSED；
    // 如果已启动，则会挨个取得其Entry，直到返回STATUS_NO_MORE_ENTRIES。	
    WDFREQUEST Request = NULL;
    do {
        status = WdfIoQueueRetrieveNextRequest(devContext->InputReportQueue, &Request);

        if (NT_SUCCESS(status))
        {
            WdfRequestComplete(Request, STATUS_SUCCESS);
        }
    } while (status != STATUS_NO_MORE_ENTRIES && status != STATUS_WDF_PAUSED);


    RegDebug(L"HidFx2EvtDeviceD0Exit end", NULL, 1);
    return status;
}



USBD_STATUS
HidFx2ValidateConfigurationDescriptor(IN PUSB_CONFIGURATION_DESCRIPTOR ConfigDesc,IN ULONG BufferLength,_Inout_  PUCHAR *Offset)
{
    USBD_STATUS status = USBD_STATUS_SUCCESS;
    USHORT ValidationLevel = 3;

    PAGED_CODE();

    status = USBD_ValidateConfigurationDescriptor( ConfigDesc, BufferLength , ValidationLevel , Offset , POOL_TAG );
    if (!(NT_SUCCESS (status)) ){
        return status;
    }

    return status;
}



VOID
HidFx2EvtInternalDeviceControl(IN WDFQUEUE Queue,IN WDFREQUEST Request,IN size_t OutputBufferLength,IN size_t InputBufferLength,IN ULONG IoControlCode)
{
    NTSTATUS            status = STATUS_SUCCESS;
    WDFDEVICE           device;
    PDEVICE_EXTENSION   devContext = NULL;
    ULONG               bytesReturned = 0;

    UNREFERENCED_PARAMETER(bytesReturned);

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    
    device = WdfIoQueueGetDevice(Queue);
    devContext = GetDeviceContext(device);

    PDEVICE_OBJECT pDevObj = WdfDeviceWdmGetDeviceObject(device);
    UNREFERENCED_PARAMETER(pDevObj);
    //RegDebug(L"HidFx2EvtInternalDeviceControl pDevObj=", pDevObj->DriverObject->DriverName.Buffer, pDevObj->DriverObject->DriverName.Length);
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
    //    RegDebug(L"IOCTL_HID_GET_INDEXED_STRING IoControlCode", NULL, IOCTL_HID_GET_INDEXED_STRING);////0xb01e2
    //    RegDebug(L"IOCTL_HID_GET_FEATURE IoControlCode", NULL, IOCTL_HID_GET_FEATURE);//0xb0192
    //    RegDebug(L"IOCTL_HID_SET_FEATURE IoControlCode", NULL, IOCTL_HID_SET_FEATURE);//0xb0191
    //    RegDebug(L"IOCTL_HID_GET_INPUT_REPORT IoControlCode", NULL, IOCTL_HID_GET_INPUT_REPORT);//0xb01a2
    //    RegDebug(L"IOCTL_HID_SET_OUTPUT_REPORT IoControlCode", NULL, IOCTL_HID_SET_OUTPUT_REPORT);//0xb0195
    //    RegDebug(L"IOCTL_HID_DEVICERESET_NOTIFICATION IoControlCode", NULL, IOCTL_HID_DEVICERESET_NOTIFICATION);//0xb0233
    //}

    //RegDebug(L"HidFx2EvtInternalDeviceControl IoControlCode", NULL, IoControlCode);

    switch (IoControlCode) {

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        RegDebug(L"IOCTL_HID_GET_DEVICE_DESCRIPTOR", NULL, runtimes_ioctl_count);
        status = HidFx2GetHidDescriptor(device, Request);
        break;
    }
        
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        RegDebug(L"IOCTL_HID_GET_DEVICE_ATTRIBUTES", NULL, runtimes_ioctl_count);
        status = HidFx2GetDeviceAttributes(Request);
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
        RegDebug(L"IOCTL_HID_GET_REPORT_DESCRIPTOR", NULL, runtimes_ioctl_count);
        status = HidFx2GetReportDescriptor(device, Request);
        break;
    }
    case IOCTL_HID_READ_REPORT: {
        RegDebug(L"HidFx2EvtInternalDeviceControl IOCTL_HID_READ_REPORT", NULL, runtimes_ioctl_count);
        DbgPrint("HidFx2EvtInternalDeviceControl IOCTL_HID_READ_REPORT runtimes_ioRead 0x%x\n",runtimes_ioctl_count);

        status = WdfRequestForwardToIoQueue(Request, devContext->InputReportQueue);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
                "WdfRequestForwardToIoQueue failed with status: 0x%x\n", status);

            RegDebug(L"HidFx2EvtInternalDeviceControl IOCTL_HID_READ_REPORT WdfRequestForwardToIoQueue failed", NULL, status);
            DbgPrint("HidFx2EvtInternalDeviceControl IOCTL_HID_READ_REPORT WdfRequestForwardToIoQueuefailed\n");

            WdfRequestComplete(Request, status);
        }

        return;
    }
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:

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
            WdfRequestComplete(Request, status);
        }

        return;

    case IOCTL_HID_SET_FEATURE:
        //status = HidFx2SetFeature(Request);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesReturned);//status??
        RegDebug(L"IOCTL_HID_SET_FEATURE ok", NULL, runtimes_ioctl_count);

        ioReady = TRUE;
        return;

    case IOCTL_HID_GET_FEATURE:
        status = PtpReportFeatures(device, Request);
        WdfRequestComplete(Request, status);

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

        status = HidGetString(devContext, Request);//代码会死机
        break;
    case IOCTL_HID_ACTIVATE_DEVICE:
        RegDebug(L"HidFx2EvtInternalDeviceControl IOCTL_HID_ACTIVATE_DEVICE", NULL, IoControlCode);
        // Makes the device ready for I/O operations.
    case IOCTL_HID_DEACTIVATE_DEVICE:
        RegDebug(L"HidFx2EvtInternalDeviceControl IOCTL_HID_DEACTIVATE_DEVICE", NULL, IoControlCode);
        // Causes the device to cease operations and terminate all outstanding
        // I/O requests.
        //
    default:
        status = STATUS_NOT_SUPPORTED;
        //RegDebug(L"HidFx2EvtInternalDeviceControl STATUS_NOT_SUPPORTED", NULL, IoControlCode);
        break;
    }

    WdfRequestComplete(Request, status);
    RegDebug(L"HidFx2EvtInternalDeviceControl end", NULL, runtimes_ioctl_count);
    return;
}



NTSTATUS
SendVendorCommand(IN WDFDEVICE Device,IN UCHAR VendorCommand,IN PUCHAR CommandData)
{
    NTSTATUS                     status = STATUS_SUCCESS;
    ULONG                        bytesTransferred = 0;
    PDEVICE_EXTENSION            pDevContext = NULL;
    WDF_MEMORY_DESCRIPTOR        memDesc;
    WDF_USB_CONTROL_SETUP_PACKET controlSetupPacket;
    WDF_REQUEST_SEND_OPTIONS     sendOptions;

    PAGED_CODE();

    pDevContext = GetDeviceContext(Device);

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
GetVendorData(IN WDFDEVICE Device,IN UCHAR VendorCommand,IN PUCHAR CommandData)
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
HidFx2GetHidDescriptor(IN WDFDEVICE Device,IN WDFREQUEST Request)
{
    NTSTATUS            status = STATUS_SUCCESS;
    size_t              bytesToCopy = 0;
    WDFMEMORY           memory;

    UNREFERENCED_PARAMETER(Device);
    // This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
    // will correctly retrieve buffer from Irp->UserBuffer.
    // Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
    // field irrespective of the ioctl buffer type. However, framework is very
    // strict about type checking. You cannot get Irp->UserBuffer by using
    // WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
    // internal ioctl.

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        RegDebug(L"HidFx2GetHidDescriptor WdfRequestRetrieveOutputMemory err", NULL, runtimes_ioctl_count);
        return status;
    }

    bytesToCopy = DefaultHidDescriptor.bLength;
    if (bytesToCopy == 0) {
        status = STATUS_INVALID_DEVICE_STATE;
        RegDebug(L"HidFx2GetHidDescriptor G_DefaultHidDescriptor is zero", NULL, status);
        return status;
    }

    status = WdfMemoryCopyFromBuffer(memory, 0, (PVOID)&DefaultHidDescriptor, bytesToCopy);
    if (!NT_SUCCESS(status)) {
        RegDebug(L"HidFx2GetHidDescriptor WdfMemoryCopyFromBuffer failed", NULL, status);
        return status;
    }

    WdfRequestSetInformation(Request, bytesToCopy);

    RegDebug(L"HidFx2GetHidDescriptor end", NULL, runtimes_ioctl_count);
    return status;
}

NTSTATUS
HidFx2GetReportDescriptor(IN WDFDEVICE Device,IN WDFREQUEST Request)
{
    NTSTATUS            status = STATUS_SUCCESS;
    ULONG_PTR           bytesToCopy;
    WDFMEMORY           memory;

    UNREFERENCED_PARAMETER(Device);

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        RegDebug(L"HidFx2GetReportDescriptor WdfRequestRetrieveOutputMemory failed", NULL, status);
        return status;
    }

    bytesToCopy = DefaultHidDescriptor.DescriptorList[0].wReportLength;

    if (bytesToCopy == 0) {
        status = STATUS_INVALID_DEVICE_STATE;
        RegDebug(L"HidFx2GetReportDescriptor G_DefaultHidDescriptor's reportLenght is zero", NULL, status);
        return status;
    }

    status = WdfMemoryCopyFromBuffer(memory, 0, (PVOID)&ParallelMode_PtpReportDescriptor, bytesToCopy);
    if (!NT_SUCCESS(status)) {
        RegDebug(L"HidFx2GetReportDescriptor WdfMemoryCopyFromBuffer failed", NULL, status);
        return status;
    }

    WdfRequestSetInformation(Request, bytesToCopy);

    RegDebug(L"HidFx2GetReportDescriptor end", NULL, runtimes_ioctl_count);
    return status;
}


NTSTATUS
HidFx2GetDeviceAttributes(IN WDFREQUEST Request)
{
    NTSTATUS                 status = STATUS_SUCCESS;
    PHID_DEVICE_ATTRIBUTES   pDeviceAttributes = NULL;
    PUSB_DEVICE_DESCRIPTOR   usbDeviceDescriptor = NULL;
    PDEVICE_EXTENSION        deviceInfo = NULL;

    deviceInfo = GetDeviceContext(WdfIoQueueGetDevice(WdfRequestGetIoQueue(Request)));

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DEVICE_ATTRIBUTES), &pDeviceAttributes, NULL);
    if (!NT_SUCCESS(status)) {
        RegDebug(L"HidFx2GetDeviceAttributes WdfRequestRetrieveOutputBuffer failed", NULL, status);
        return status;
    }

    // Retrieve USB device descriptor saved in device context
    usbDeviceDescriptor = WdfMemoryGetBuffer(deviceInfo->DeviceDescriptor, NULL);

    pDeviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
    pDeviceAttributes->VendorID = usbDeviceDescriptor->idVendor;
    pDeviceAttributes->ProductID = usbDeviceDescriptor->idProduct;;
    pDeviceAttributes->VersionNumber = usbDeviceDescriptor->bcdDevice;

    RegDebug(L"HidFx2GetDeviceAttributes VendorID", NULL, usbDeviceDescriptor->idVendor);
    RegDebug(L"HidFx2GetDeviceAttributes ProductID", NULL, usbDeviceDescriptor->idProduct);
    RegDebug(L"HidFx2GetDeviceAttributes VersionNumber", NULL, usbDeviceDescriptor->bcdDevice);

    WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

    RegDebug(L"HidFx2GetDeviceAttributes end", NULL, runtimes_ioctl_count);
    return status;
}


NTSTATUS
HidFx2SendIdleNotification(IN WDFREQUEST Request)
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

    // Convert the request to corresponding USB Idle notification request
    if (currentIrpStack->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO)) {

        status = STATUS_BUFFER_TOO_SMALL;
        RegDebug(L"HidFx2SendIdleNotification STATUS_BUFFER_TOO_SMALL", NULL, status);
        return status;
    }

    ASSERT(sizeof(HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO)
        == sizeof(USB_IDLE_CALLBACK_INFO));

#pragma warning(suppress :4127)  // conditional expression is constant warning
    if (sizeof(HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO) != sizeof(USB_IDLE_CALLBACK_INFO)) {

        status = STATUS_INFO_LENGTH_MISMATCH;
        RegDebug(L"HidFx2SendIdleNotification STATUS_INFO_LENGTH_MISMATCH", NULL, status);
        return status;
    }

    //
    // prepare next stack location
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


NTSTATUS
PtpReportFeatures(_In_ WDFDEVICE Device,_In_ WDFREQUEST Request)
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
HidGetString(PDEVICE_EXTENSION pDevContext,WDFREQUEST Request)
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
Filter_DispatchPassThrough(_In_ WDFREQUEST Request,_In_ WDFIOTARGET Target)
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
DriverEntry(_In_ PDRIVER_OBJECT  DriverObject,  _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS               status = STATUS_SUCCESS;
    WDF_DRIVER_CONFIG      config;
    WDF_OBJECT_ATTRIBUTES  attributes;

    // Initialize WPP Tracing
    WPP_INIT_TRACING(DriverObject, RegistryPath);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
        "HIDUSBFX2 Driver Sample\n");

    RegDebug(L"DriverEntry start", NULL, status);

    WDF_DRIVER_CONFIG_INIT(&config, HidFx2EvtDeviceAdd);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = HidFx2EvtDriverContextCleanup;

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
HidFx2EvtDeviceAdd(IN WDFDRIVER Driver, IN PWDFDEVICE_INIT DeviceInit)

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

    WdfFdoInitSetFilter(DeviceInit);
    WdfPdoInitAllowForwardingRequestToParent(DeviceInit);//新增

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

    pnpPowerCallbacks.EvtDevicePrepareHardware = HidFx2EvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = ReleaseHardware;

    pnpPowerCallbacks.EvtDeviceD0Entry = HidFx2EvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = HidFx2EvtDeviceD0Exit;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_EXTENSION);

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
    queueConfig.EvtIoStop = HidFx2EvtIoStop;

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


    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(hDevice,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &devContext->InputReportQueue
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfIoQueueCreate failed 0x%x\n", status);
        RegDebug(L"HidFx2EvtDeviceAdd WdfIoQueueCreate InputReportQueue failed", NULL, status);
        return status;
    }

    return status;
}


VOID
HidFx2EvtDriverContextCleanup(
    IN WDFOBJECT Object
)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Object);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Exit HidFx2EvtDriverContextCleanup\n");

    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)Object));
}


VOID
HidFx2EvtIoStop(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ ULONG ActionFlags)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(ActionFlags);

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
SelectInterruptInterface(_In_ WDFDEVICE Device)
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
    for (index = 0; index < numberConfiguredPipes; index++) {

        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

        pipe = WdfUsbInterfaceGetConfiguredPipe(
            pDeviceContext->UsbInterface,
            index, //PipeIndex,
            &pipeInfo
        );

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

    // If we didn't find interrupt pipe, fail the start.
    if (!pDeviceContext->InterruptPipe) {
        status = STATUS_INVALID_DEVICE_STATE;
        RegDebug(L"SelectInterruptInterface STATUS_INVALID_DEVICE_STATE", NULL, status);
        return status;
    }

    RegDebug(L"SelectInterruptInterface end", NULL, status);
    return status;
}



BOOLEAN
HidFx2EvtUsbInterruptReadersFailed(_In_ WDFUSBPIPE Pipe, _In_ NTSTATUS Status, _In_ USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(UsbdStatus);
    UNREFERENCED_PARAMETER(Status);

    //RegDebug(L"AmtPtpEvtUsbInterruptReadersFailed end", NULL, runtimes_ioRead);
    return TRUE;
}


NTSTATUS 
StartAllPipes(_In_ PDEVICE_EXTENSION DeviceContext)
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
StopAllPipes(_In_ PDEVICE_EXTENSION DeviceContext)
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
DispatchRead(IN PDEVICE_EXTENSION devContext)
{
    KdPrint(("DispatchRead start %x", runtimes_ioRead));
    NTSTATUS Status = STATUS_SUCCESS;
    WDFREQUEST ptpRequest;


    struct mouse_report_t mreport;
    size_t bytesToCopy = 0;
    size_t bytesReturned = 0;

    // Retrieve next PTP touchpad request.
    Status = WdfIoQueueRetrieveNextRequest(
        devContext->InputReportQueue,
        &ptpRequest
    );

    if (!NT_SUCCESS(Status)) {
        KdPrint(("DispatchRead WdfIoQueueRetrieveNextRequest err %x", Status));
        return;
    }

    Status = WdfRequestRetrieveOutputBuffer(ptpRequest,
        bytesToCopy,
        (PVOID*)&mreport,
        &bytesReturned);// BufferLength

    if (!NT_SUCCESS(Status)) {
        KdPrint(("DispatchRead WdfRequestRetrieveOutputBuffer err %x", Status));
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

    WdfRequestCompleteWithInformation(ptpRequest, Status, bytesReturned);//NumBytesTransferred

    //// Set result
    //WdfRequestSetInformation(ptpRequest, sizeof(struct mouse_report_t));//NumBytesTransferred//sizeof(PTP_REPORT)

    //// Set completion flag
    //WdfRequestComplete(ptpRequest, Status);


    KdPrint(("DispatchRead end %x", runtimes_ioRead));
}


VOID
DispatchReadRequest(IN PDEVICE_EXTENSION devContext, ULONG NumBytesTransferred)
{
    KdPrint(("DispatchReadRequest start%x", runtimes_ioRead));

    NTSTATUS Status = STATUS_SUCCESS;
    WDFREQUEST ptpRequest;
    WDFMEMORY  RequestMemory;

    // Retrieve next PTP touchpad request.
    Status = WdfIoQueueRetrieveNextRequest(
        devContext->InputReportQueue,
        &ptpRequest
    );

    if (!NT_SUCCESS(Status)) {
        KdPrint(("DispatchReadRequest WdfIoQueueRetrieveNextRequest err %x", Status));
        return;
    }

    Status = WdfRequestRetrieveOutputMemory(
        ptpRequest,
        &RequestMemory
    );

    if (!NT_SUCCESS(Status)) {
        KdPrint(("DispatchReadRequest WdfRequestRetrieveOutputMemory err %x", runtimes_ioRead));
        return;
    }


    //PTP_REPORT PtpReport;

    struct mouse_report_t mreport;
    RtlZeroMemory(&mreport, sizeof(mreport));

    //if (NumBytesTransferred >= sizeof(PTP_REPORT)) {

    //    RtlCopyBytes(&PtpReport, TouchBuffer, sizeof(PTP_REPORT));//
    //}

    mreport.button = 0;
    mreport.dx = 1;
    mreport.dy = 1;
    mreport.v_wheel = 0;
    mreport.report_id = FAKE_REPORTID_MOUSE;

    KdPrint(("DispatchReadRequest mreport.dx= %x, dy= %x\n", mreport.dx, mreport.dy));

    // Compose final report and write it back
    Status = WdfMemoryCopyFromBuffer(
        RequestMemory,
        0,
        (PVOID)&mreport,
        sizeof(struct mouse_report_t)
    );

    if (!NT_SUCCESS(Status)) {
        KdPrint(("DispatchReadRequest WdfMemoryCopyFromBuffer err %x", Status));
        return;
    }

    // Set result
    WdfRequestSetInformation(ptpRequest, NumBytesTransferred);//sizeof(struct mouse_report_t)//sizeof(PTP_REPORT)

    // Set completion flag
    WdfRequestComplete(ptpRequest, Status);

    KdPrint(("DispatchReadRequest end %x", runtimes_ioRead));


}
