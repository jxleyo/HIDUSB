#include "HID_USB.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, HumGetHidDescriptor)
#pragma alloc_text(PAGE, HumGetReportDescriptor)
#pragma alloc_text(PAGE, HumGetStringDescriptor)
#pragma alloc_text(PAGE, HumGetPhysicalDescriptor)
#pragma alloc_text(PAGE, HumGetDeviceAttributes)
#pragma alloc_text(PAGE, HumGetMsGenreDescriptor)
#endif

//KSPIN_LOCK resetWorkItemsListSpinLock = 0;
LARGE_INTEGER UrbTimeout = { 0xFD050F80, 0xFFFFFFFF };

ULONG runtimes_step;
ULONG runtimes_ioctl;
ULONG runtimes_readinput;

#define debug_on 1

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


PVOID
HumGetSystemAddressForMdlSafe(PMDL MdlAddress)
{
    PVOID buf = NULL;
    /*
     *  Can't call MmGetSystemAddressForMdlSafe in a WDM driver,
     *  so set the MDL_MAPPING_CAN_FAIL bit and check the result
     *  of the mapping.
     */
    if (MdlAddress) {
        MdlAddress->MdlFlags |= MDL_MAPPING_CAN_FAIL;
        buf = MmGetSystemAddressForMdl(MdlAddress);
        MdlAddress->MdlFlags &= ~(MDL_MAPPING_CAN_FAIL);
    }
    else {
        ASSERT(MdlAddress);
    }
    return buf;
}


PUSBD_PIPE_INFORMATION GetInterruptInputPipeForDevice(PDEVICE_EXTENSION DeviceExtension)
{
    ULONG                       Index;
    ULONG                       NumOfPipes;
    PUSBD_PIPE_INFORMATION      pPipeInfo = NULL;
    PUSBD_INTERFACE_INFORMATION pInterfaceInfo;

    pInterfaceInfo = DeviceExtension->pInterfaceInfo;

    Index = 0;
    NumOfPipes = pInterfaceInfo->NumberOfPipes;
    if (NumOfPipes == 0)
    {
        return NULL;
    }
    
    while (1)
    {
        pPipeInfo = &pInterfaceInfo->Pipes[Index];
        if ((USB_ENDPOINT_DIRECTION_IN(pPipeInfo->EndpointAddress)) && (pPipeInfo->PipeType == UsbdPipeTypeInterrupt)) {
            break;
        }
        ++Index;

        if (Index >= NumOfPipes)
        {
            return NULL;
        }
    }
    return pPipeInfo;
}

PUSBD_PIPE_INFORMATION GetInterruptOutputPipeForDevice(PDEVICE_EXTENSION DeviceExtension)
{
    ULONG                       Index;
    ULONG                       NumOfPipes;
    PUSBD_PIPE_INFORMATION      pPipeInfo = NULL;
    PUSBD_INTERFACE_INFORMATION pInterfaceInfo;

    pInterfaceInfo = DeviceExtension->pInterfaceInfo;
    Index = 0;
    NumOfPipes = pInterfaceInfo->NumberOfPipes;
    if (NumOfPipes == 0)
    {
        return NULL;
    }

    while (1)
    {
        pPipeInfo = &pInterfaceInfo->Pipes[Index];
        if ((USB_ENDPOINT_DIRECTION_OUT(pPipeInfo->EndpointAddress)) && (pPipeInfo->PipeType == UsbdPipeTypeInterrupt)) {
            break;
        }
        ++Index;

        if (Index >= NumOfPipes)
        {
            return NULL;
        }
    }
    return pPipeInfo;

    //ULONG i;
    //PUSBD_PIPE_INFORMATION pipeInfo = NULL;


    //for (i = 0; i < DeviceExtension->pInterfaceInfo->NumberOfPipes; i++) {
    //    UCHAR endPtAddr = DeviceExtension->pInterfaceInfo->Pipes[i].EndpointAddress;
    //    USBD_PIPE_TYPE pipeType = DeviceExtension->pInterfaceInfo->Pipes[i].PipeType;

    //    if (!(endPtAddr & USB_ENDPOINT_DIRECTION_MASK) && (pipeType == UsbdPipeTypeInterrupt)) {
    //        pipeInfo = &DeviceExtension->pInterfaceInfo->Pipes[i];
    //        break;
    //    }
    //}

    //return pipeInfo;
}


NTSTATUS HumInternalIoctl(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{
    NTSTATUS                ntStatus = STATUS_SUCCESS;
    PHID_DEVICE_EXTENSION   pDevExt;
    PDEVICE_EXTENSION       pMiniDevExt;
    PIO_STACK_LOCATION      pStack;
    ULONG                   IoControlCode;
    BOOLEAN                 NeedsCompletion = TRUE;
    BOOLEAN                 AcquiredLock = FALSE;

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

    ++runtimes_step;
    ++runtimes_ioctl;
    //RegDebug(L"HumInternalIoctl runtimes_step", NULL, runtimes_step);
    //RegDebug(L"HumInternalIoctl runtimes_ioctl", NULL, runtimes_ioctl);

    pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
    pMiniDevExt = (PDEVICE_EXTENSION)pDevExt->MiniDeviceExtension;

    ntStatus = IoAcquireRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, __FILE__, __LINE__, sizeof(pMiniDevExt->RemoveLock));
    if (NT_SUCCESS(ntStatus) == FALSE)
    {
        goto Exit;
    }

    pStack = IoGetCurrentIrpStackLocation(pIrp);
    AcquiredLock = TRUE;
    IoControlCode = pStack->Parameters.DeviceIoControl.IoControlCode;

    switch (IoControlCode) {

        case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
            RegDebug(L"HumInternalIoctl IOCTL_HID_GET_DEVICE_DESCRIPTOR", NULL, runtimes_ioctl);
            /*
             *  This IOCTL uses buffering method METHOD_NEITHER,
             *  so the buffer is pIrp->UserBuffer.
             */
            ntStatus = HumGetHidDescriptor(pDeviceObject, pIrp);
            break;

        case IOCTL_HID_GET_REPORT_DESCRIPTOR:
            RegDebug(L"HumInternalIoctl IOCTL_HID_GET_REPORT_DESCRIPTOR", NULL, runtimes_ioctl);
            /*
             *  This IOCTL uses buffering method METHOD_NEITHER,
             *  so the buffer is pIrp->UserBuffer.
             */
            ntStatus = HumGetReportDescriptor(pDeviceObject, pIrp, &NeedsCompletion);
            break;

        case IOCTL_HID_READ_REPORT:
            //RegDebug(L"HumInternalIoctl IOCTL_HID_READ_REPORT", NULL, runtimes_ioctl); 
            /*
             *  This IOCTL uses buffering method METHOD_NEITHER,
             *  so the buffer is pIrp->UserBuffer.
             */
            ntStatus = HumReadReport(pDeviceObject, pIrp, &NeedsCompletion);
            break;

        case IOCTL_HID_WRITE_REPORT:
            //RegDebug(L"HumInternalIoctl IOCTL_HID_WRITE_REPORT", NULL, runtimes_ioctl);
            /*
             *  This IOCTL uses buffering method METHOD_NEITHER,
             *  so the buffer is pIrp->UserBuffer.
             */
            ntStatus = HumWriteReport(pDeviceObject, pIrp, &NeedsCompletion);
            break;

        case IOCTL_HID_GET_STRING:
            RegDebug(L"HumInternalIoctl IOCTL_HID_GET_STRING", NULL, runtimes_ioctl);
            /*
             *  Get the friendly name for the device.
             *
             *  This IOCTL uses buffering method METHOD_NEITHER,
             *  so the buffer is pIrp->UserBuffer.
             */
            ntStatus = HumGetStringDescriptor(pDeviceObject, pIrp);
            break;

        case IOCTL_HID_GET_INDEXED_STRING:
            RegDebug(L"HumInternalIoctl IOCTL_HID_GET_INDEXED_STRING", NULL, runtimes_ioctl);
            ntStatus = HumGetStringDescriptor(pDeviceObject, pIrp);
            break;

        case IOCTL_HID_SET_FEATURE:
            RegDebug(L"HumInternalIoctl IOCTL_HID_SET_FEATURE", NULL, runtimes_ioctl);
            ntStatus = HumGetSetReport(pDeviceObject, pIrp, &NeedsCompletion);
            break;
        case IOCTL_HID_GET_FEATURE:
            RegDebug(L"HumInternalIoctl IOCTL_HID_GET_FEATURE", NULL, runtimes_ioctl);
            ntStatus = HumGetSetReport(pDeviceObject, pIrp, &NeedsCompletion);
            break;
        case IOCTL_HID_GET_INPUT_REPORT:
            RegDebug(L"HumInternalIoctl IOCTL_HID_GET_INPUT_REPORT", NULL, runtimes_ioctl);
            ntStatus = HumGetSetReport(pDeviceObject, pIrp, &NeedsCompletion);
            break;
        case IOCTL_HID_SET_OUTPUT_REPORT:
            RegDebug(L"HumInternalIoctl IOCTL_HID_SET_OUTPUT_REPORT", NULL, runtimes_ioctl);
            ntStatus = HumGetSetReport(pDeviceObject, pIrp, &NeedsCompletion);
            break;

        case IOCTL_HID_ACTIVATE_DEVICE:
            RegDebug(L"HumInternalIoctl IOCTL_HID_ACTIVATE_DEVICE", NULL, runtimes_ioctl);
            /*
             *  We don't do anything for these IOCTLs but some minidrivers might.
             */
            ntStatus = STATUS_SUCCESS;
            break;
        case IOCTL_HID_DEACTIVATE_DEVICE:
            RegDebug(L"HumInternalIoctl IOCTL_HID_DEACTIVATE_DEVICE", NULL, runtimes_ioctl);
            /*
             *  We don't do anything for these IOCTLs but some minidrivers might.
             */
            ntStatus = STATUS_SUCCESS;
            break;

        case IOCTL_GET_PHYSICAL_DESCRIPTOR:
            RegDebug(L"HumInternalIoctl IOCTL_GET_PHYSICAL_DESCRIPTOR", NULL, runtimes_ioctl);
            /*
             *  This IOCTL gets information related to the human body part used
             *  to control a device control.
             */
            ntStatus = HumGetPhysicalDescriptor(pDeviceObject, pIrp, &NeedsCompletion);
            break;

        case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
            RegDebug(L"HumInternalIoctl IOCTL_HID_GET_DEVICE_ATTRIBUTES", NULL, runtimes_ioctl);
            /*
             *  This IOCTL uses buffering method METHOD_NEITHER,
             *  so the buffer is pIrp->UserBuffer.
             *  If the pIrp is coming to us from user space,
             *  we must validate the buffer.
             */
            ntStatus = HumGetDeviceAttributes(pDeviceObject, pIrp);
            break;

        case IOCTL_HID_GET_MS_GENRE_DESCRIPTOR:
            RegDebug(L"HumInternalIoctl IOCTL_HID_GET_MS_GENRE_DESCRIPTOR", NULL, runtimes_ioctl);
            /*
             *  This IOCTL uses buffering method METHOD_NEITHER,
             *  so the buffer is pIrp->UserBuffer.
             *  If the pIrp is coming to us from user space,
             *  we must validate the buffer.
             */
            ntStatus = HumGetMsGenreDescriptor(pDeviceObject, pIrp);
            break;

        case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
            RegDebug(L"HumInternalIoctl IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST", NULL, runtimes_ioctl);
            ntStatus = HumSendIdleNotificationRequest(pDeviceObject, pIrp, &NeedsCompletion, &AcquiredLock);
            break;


       /* case IOCTL_HID_DEVICERESET_NOTIFICATION:
        {
            RegDebug(L"HumInternalIoctl IOCTL_HID_DEVICERESET_NOTIFICATION", NULL, runtimes_ioctl);
            KIRQL newIRQL = KeAcquireSpinLockRaiseToDpc(&pMiniDevExt->DeviceResetNotificationSpinLock);
            if (pMiniDevExt->pDeviceResetNotificationIrp)
            {
                KeReleaseSpinLock(&pMiniDevExt->DeviceResetNotificationSpinLock, newIRQL);
                ntStatus = STATUS_INVALID_DEVICE_REQUEST;
                AcquiredLock = TRUE;
                goto Exit;
            }
            pMiniDevExt->pDeviceResetNotificationIrp = pIrp;
            InterlockedExchange64((PVOID)pIrp->CancelRoutine, (LONG64)HumDeviceResetNotificationIrpCancelled);
            if ((pIrp->Cancel) && InterlockedExchange64((PVOID)pIrp->CancelRoutine, 0))
            {
                pMiniDevExt->pDeviceResetNotificationIrp = NULL;
                KeReleaseSpinLock(&pMiniDevExt->DeviceResetNotificationSpinLock, newIRQL);
                ntStatus = STATUS_PENDING;

                pIrp->IoStatus.Status = STATUS_CANCELLED;
                IofCompleteRequest(pIrp, 0);
                goto Exit;
            }

            IoMarkIrpPending(pIrp);
            KeReleaseSpinLock(&pMiniDevExt->DeviceResetNotificationSpinLock, newIRQL);
            ntStatus = STATUS_PENDING; 
        }
            break;*/

        default:
            RegDebug(L"HumInternalIoctl default UNKOWN IOCTL", NULL, runtimes_ioctl);
            /*
             *  Note: do not return STATUS_NOT_SUPPORTED;
             *  Just keep the default status (this allows filter drivers to work).
             */
            ntStatus = pIrp->IoStatus.Status;
            break;
    }



Exit:
    /*
     *  Complete the pIrp only if we did not pass it to a lower driver.
     */
    if (NeedsCompletion) {
        pIrp->IoStatus.Status = ntStatus;
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    }

    if (AcquiredLock)
    {
        IoReleaseRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, sizeof(pMiniDevExt->RemoveLock));
    }

    RegDebug(L"HumInternalIoctl end", NULL, runtimes_ioctl);
    return ntStatus;
}



NTSTATUS HumPower(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{
    NTSTATUS                status;
    PHID_DEVICE_EXTENSION   pDevExt;
    PDEVICE_EXTENSION pMiniDevExt;

    pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
    pMiniDevExt = (PDEVICE_EXTENSION)pDevExt->MiniDeviceExtension;

    status = IoAcquireRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, __FILE__, __LINE__, sizeof(pMiniDevExt->RemoveLock));
    if (NT_SUCCESS(status) == FALSE)
    {
        PoStartNextPowerIrp(pIrp);
        pIrp->IoStatus.Status = status;
        pIrp->IoStatus.Information = 0;
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    }
    else
    {
        IoCopyCurrentIrpStackLocationToNext(pIrp);
        IoSetCompletionRoutine(pIrp, (PIO_COMPLETION_ROUTINE)HumPowerCompletion, NULL, TRUE, TRUE, TRUE);
        status = PoCallDriver(pDevExt->NextDeviceObject, pIrp);
    }

    return status;
}

NTSTATUS HumReadReport(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, BOOLEAN* NeedsCompletion)
{
    NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;
    PHID_DEVICE_EXTENSION   pDevExt;
    PDEVICE_EXTENSION pMiniDevExt;

    PIO_STACK_LOCATION IrpStack;
    PVOID ReportBuffer;
    ULONG ReportTotalSize;
    PIO_STACK_LOCATION NextStack;
    PURB pUrb;

    pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
    pMiniDevExt = (PDEVICE_EXTENSION)pDevExt->MiniDeviceExtension;
    IrpStack = IoGetCurrentIrpStackLocation(pIrp);

    ReportBuffer = pIrp->UserBuffer;
    ReportTotalSize = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;

    runtimes_readinput++;
    //RegDebug(L"HumReadReport start", NULL, runtimes_readinput);

    if (!ReportTotalSize || !ReportBuffer) {
        RegDebug(L"HumReadReport STATUS_INVALID_PARAMETER", NULL, runtimes_readinput);
        return STATUS_INVALID_PARAMETER;
    }
    PUSBD_PIPE_INFORMATION inputInterruptPipe;

    inputInterruptPipe = GetInterruptInputPipeForDevice(pMiniDevExt);
    if (!inputInterruptPipe) {
        RegDebug(L"HumReadReport STATUS_DEVICE_CONFIGURATION_ERROR", NULL, runtimes_readinput);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    //RegDebug(L"HumReadReport inputInterruptPipe ok", NULL, runtimes_readinput);
    pUrb = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(URB), HIDUSB_TAG);
    if (pUrb) {
        //RegDebug(L"HumReadReport pUrb ok", NULL, runtimes_readinput);
        //RtlZeroMemory(pUrb, sizeof(URB));

        pUrb->UrbBulkOrInterruptTransfer.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
        pUrb->UrbBulkOrInterruptTransfer.Hdr.Length = sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);

        pUrb->UrbBulkOrInterruptTransfer.PipeHandle = inputInterruptPipe->PipeHandle;

        pUrb->UrbBulkOrInterruptTransfer.TransferBufferLength = ReportTotalSize;
        pUrb->UrbBulkOrInterruptTransfer.TransferBufferMDL = NULL;
        pUrb->UrbBulkOrInterruptTransfer.TransferBuffer = ReportBuffer;
        pUrb->UrbBulkOrInterruptTransfer.TransferFlags = USBD_SHORT_TRANSFER_OK | USBD_TRANSFER_DIRECTION_IN;
        pUrb->UrbBulkOrInterruptTransfer.UrbLink = NULL;

        IoSetCompletionRoutine(pIrp, (PIO_COMPLETION_ROUTINE)HumReadCompletion, pUrb, TRUE, TRUE, TRUE);    // context

        NextStack = IoGetNextIrpStackLocation(pIrp);
        NextStack->Parameters.Others.Argument1 = pUrb;
        NextStack->MajorFunction = IrpStack->MajorFunction;

        NextStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;

        NextStack->DeviceObject = GET_NEXT_DEVICE_OBJECT(pDeviceObject);

        if (pMiniDevExt->PnpState == DEVICE_STATE_RUNNING || pMiniDevExt->PnpState == DEVICE_STATE_STARTING)
        {
            //RegDebug(L"HumReadReport PnpState ok", NULL, runtimes_readinput);
            ntStatus = IoCallDriver(pDevExt->NextDeviceObject, pIrp);
            *NeedsCompletion = FALSE;
        }
        else {
            HumDecrementPendingRequestCount(pMiniDevExt);
            ExFreePool(pUrb);
            RegDebug(L"HumReadReport STATUS_NO_SUCH_DEVICE", NULL, runtimes_readinput);
            ntStatus = STATUS_NO_SUCH_DEVICE;
        }
    }
    else {
        RegDebug(L"HumReadReport STATUS_INSUFFICIENT_RESOURCES", NULL, runtimes_readinput);
        ntStatus = STATUS_INSUFFICIENT_RESOURCES;
    }


    return ntStatus;
}


NTSTATUS HumReadCompletion(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, IN PVOID Context)
{
    NTSTATUS ntStatus = STATUS_SUCCESS;
    PURB pUrb;
    ULONG bytesRead;
    PDEVICE_EXTENSION pMiniDevExt;

    pMiniDevExt = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
    //RegDebug(L"HumReadCompletion start", NULL, runtimes_readinput);
    //
    // We passed a pointer to the URB as our context, get it now.
    //
    pUrb = (PURB)Context;

    ntStatus = pIrp->IoStatus.Status;
    if (NT_SUCCESS(ntStatus)) {
        //
        // Get the bytes read and store in the status block
        //

        bytesRead = pUrb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        pIrp->IoStatus.Information = bytesRead;
        //RegDebug(L"HumReadCompletion TransferBuffer=", pUrb->UrbBulkOrInterruptTransfer.TransferBuffer, bytesRead);
        goto Exit;
    }

    if (ntStatus == STATUS_CANCELLED) {
        //The IRP was cancelled, which means that the device is probably getting removed.
        DBGPRINT(2, ("Read irp %p cancelled ...", pIrp));
    }
    else{
        if (ntStatus == STATUS_DEVICE_NOT_CONNECTED) {
            goto Exit;
        }

        ntStatus = HumQueueResetWorkItem(pDeviceObject, pIrp);
        
    }

Exit:

    ExFreePool(pUrb);
    if (InterlockedDecrement(&pMiniDevExt->nPendingRequestsCount) < 0)
    {
        KeSetEvent(&pMiniDevExt->AllRequestsCompleteEvent, IO_NO_INCREMENT, FALSE);
    }
    IoReleaseRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, sizeof(pMiniDevExt->RemoveLock));
    return ntStatus;
}


NTSTATUS HumWriteReport(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, BOOLEAN* NeedsCompletion)
{
    NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;
    PDEVICE_EXTENSION DeviceExtension;
    PIO_STACK_LOCATION currentIrpStack, nextIrpStack;
    PURB pUrb;

    PHID_XFER_PACKET hidWritePacket;

    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
    currentIrpStack = IoGetCurrentIrpStackLocation(pIrp);
    nextIrpStack = IoGetNextIrpStackLocation(pIrp);

    hidWritePacket = (PHID_XFER_PACKET)pIrp->UserBuffer;
    if (!hidWritePacket || !hidWritePacket->reportBuffer || !hidWritePacket->reportBufferLen) {
        return STATUS_DATA_ERROR;
    }

    pUrb = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(URB), HIDUSB_TAG);
    if (!pUrb) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(pUrb, sizeof(URB));
    PUSBD_PIPE_INFORMATION interruptPipe;

    if (DeviceExtension->DeviceFlags & DEVICE_FLAGS_HID_1_0_D3_COMPAT_DEVICE) {
        interruptPipe = GetInterruptInputPipeForDevice(DeviceExtension);
        if (!interruptPipe) {
            ExFreePool(pUrb);
            return STATUS_DATA_ERROR;
        }

        UCHAR deviceInputEndpoint = interruptPipe->EndpointAddress & ~USB_ENDPOINT_DIRECTION_MASK;

        /*
         *   A control operation consists of 3 stages: setup, data, and status.
         *   In the setup stage the device receives an 8-byte frame comprised of
         *   the following fields of a _URB_CONTROL_VENDOR_OR_CLASS_REQUEST structure:
         *   See section 7.2 in the USB HID specification for how to fill out these fields.
         *
         *      UCHAR RequestTypeReservedBits;
         *      UCHAR Request;
         *      USHORT Value;
         *      USHORT Index;
         *
         */
        HumBuildClassRequest(
            pUrb,
            URB_FUNCTION_CLASS_ENDPOINT,
            0,                  // transferFlags,
            hidWritePacket->reportBuffer,
            hidWritePacket->reportBufferLen,
            0x22,               // requestType= Set_Report Request,
            0x09,               // request=SET_REPORT,
            (0x0200 + hidWritePacket->reportId), // value= reportType 'output' &reportId,
            deviceInputEndpoint, // index= interrupt input endpoint for this device
            hidWritePacket->reportBufferLen    // reqLength (not used)
        );
    }
    else{

        interruptPipe = GetInterruptOutputPipeForDevice(DeviceExtension);
        if (interruptPipe) {
            /*
                *  This device has an interrupt output pipe.
                */

            pUrb->UrbBulkOrInterruptTransfer.Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
            pUrb->UrbBulkOrInterruptTransfer.Hdr.Length = sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);

            pUrb->UrbBulkOrInterruptTransfer.PipeHandle = interruptPipe->PipeHandle;

            pUrb->UrbBulkOrInterruptTransfer.TransferBufferLength = hidWritePacket->reportBufferLen;
            pUrb->UrbBulkOrInterruptTransfer.TransferBufferMDL = NULL;
            pUrb->UrbBulkOrInterruptTransfer.TransferBuffer = hidWritePacket->reportBuffer;
            pUrb->UrbBulkOrInterruptTransfer.TransferFlags = USBD_SHORT_TRANSFER_OK | USBD_TRANSFER_DIRECTION_OUT;
            pUrb->UrbBulkOrInterruptTransfer.UrbLink = NULL;
        }
        else {
            /*
                *  This device does not have an interrupt output pipe.
                *  Send the report on the control pipe.
                */

                /*
                *   A control operation consists of 3 stages: setup, data, and status.
                *   In the setup stage the device receives an 8-byte frame comprised of
                *   the following fields of a _URB_CONTROL_VENDOR_OR_CLASS_REQUEST structure:
                *   See section 7.2 in the USB HID specification for how to fill out these fields.
                *
                *      UCHAR RequestTypeReservedBits;
                *      UCHAR Request;
                *      USHORT Value;
                *      USHORT Index;
                *
                */
            HumBuildClassRequest(
                pUrb,
                URB_FUNCTION_CLASS_INTERFACE,
                0,                  // transferFlags,
                hidWritePacket->reportBuffer,
                hidWritePacket->reportBufferLen,
                0x22,               // requestType= Set_Report Request,
                0x09,               // request=SET_REPORT,
                (0x0200 + hidWritePacket->reportId), // value= reportType 'output' &reportId,
                DeviceExtension->pInterfaceInfo->InterfaceNumber, // index= interrupt input interface for this device
                hidWritePacket->reportBufferLen    // reqLength (not used)
            );
        }
    }

    IoSetCompletionRoutine(pIrp, HumWriteCompletion, pUrb, TRUE, TRUE, TRUE);

    nextIrpStack->Parameters.Others.Argument1 = pUrb;
    nextIrpStack->MajorFunction = currentIrpStack->MajorFunction;
    nextIrpStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    nextIrpStack->DeviceObject = GET_NEXT_DEVICE_OBJECT(pDeviceObject);

    //
    // We need to keep track of the number of pending requests
    // so that we can make sure they're all cancelled properly during
    // processing of a stop device request.
    //

    if (NT_SUCCESS(HumIncrementPendingRequestCount(DeviceExtension))) {

        ntStatus = IoCallDriver(GET_NEXT_DEVICE_OBJECT(pDeviceObject), pIrp);

        *NeedsCompletion = FALSE;

    }
    else {
        ExFreePool(pUrb);

        ntStatus = STATUS_NO_SUCH_DEVICE;
    }

    return ntStatus;
}


NTSTATUS HumWriteCompletion(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, IN PVOID Context)
{
    PURB pUrb = (PURB)Context;
    PDEVICE_EXTENSION deviceExtension;

    deviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);

    if (NT_SUCCESS(pIrp->IoStatus.Status)) {
        //
        //  Record the number of bytes written.
        //
        pIrp->IoStatus.Information = (ULONG)pUrb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    }

    ExFreePool(pUrb);

    /*
     *  If the lower driver returned PENDING, mark our stack location as
     *  pending also. This prevents the IRP's thread from being freed if
     *  the client's call returns pending.
     */
    if (pIrp->PendingReturned) {
        IoMarkIrpPending(pIrp);
    }

    /*
     *  Balance the increment we did when we issued the write.
     */
    HumDecrementPendingRequestCount(deviceExtension);

    return STATUS_SUCCESS;
}

NTSTATUS HumPnpCompletion(IN PDEVICE_OBJECT pDeviceObject,
                          IN PIRP           pIrp,
                          IN PVOID          Context)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    if (pIrp->PendingReturned) {
        IoMarkIrpPending(pIrp);
    }

    KeSetEvent((PKEVENT)Context, EVENT_INCREMENT, FALSE);
    // No special priority
    // No Wait

    return STATUS_MORE_PROCESSING_REQUIRED; // Keep this pIRP
}


NTSTATUS HumPowerCompletion(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{

    NTSTATUS status;
    PDEVICE_EXTENSION pMiniDevExt;
    PIO_WORKITEM            pWorkItem;

    pMiniDevExt = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);

    status = pIrp->IoStatus.Status;

    if (pIrp->PendingReturned) {
        IoMarkIrpPending(pIrp);
    }

    if (NT_SUCCESS(status)) {
        PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(pIrp);

        if ((irpSp->MinorFunction == IRP_MN_SET_POWER) && (irpSp->Parameters.Power.Type == DevicePowerState)\
            && (irpSp->Parameters.Power.State.DeviceState == PowerDeviceD0) && (pMiniDevExt->PnpState == DEVICE_STATE_RUNNING)) {

            if (KeGetCurrentIrql() < DISPATCH_LEVEL)
            {
                HumSetIdle(pDeviceObject);
            }
            else
            {
                pWorkItem = IoAllocateWorkItem(pDeviceObject);
                if (pWorkItem)
                {
                    IoQueueWorkItem(pWorkItem, (PIO_WORKITEM_ROUTINE)HumSetIdleWorker, CriticalWorkQueue, (PVOID)pWorkItem);
                }
            }
        }   
    }

    IoReleaseRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, sizeof(pMiniDevExt->RemoveLock));
    return STATUS_SUCCESS;
}

NTSTATUS
HumSendIdleNotificationRequest(
    PDEVICE_OBJECT pDeviceObject,
    PIRP pIrp,
    BOOLEAN* NeedsCompletion,
    BOOLEAN* AcquiredLock
)
{
    NTSTATUS ntStatus;
    PIO_STACK_LOCATION pStack, pNextStack;

    PHID_DEVICE_EXTENSION pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
    PDEVICE_EXTENSION pMiniDevExt = (PDEVICE_EXTENSION)pDevExt->MiniDeviceExtension;
    pStack = IoGetCurrentIrpStackLocation(pIrp);

    if (pStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_SUBMIT_IDLE_NOTIFICATION_CALLBACK_INFO)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    IoReleaseRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, sizeof(pMiniDevExt->RemoveLock));
    pNextStack = IoGetNextIrpStackLocation(pIrp);

    *NeedsCompletion = FALSE;
    pNextStack->MajorFunction = pStack->MajorFunction;
    pNextStack->Parameters.DeviceIoControl.InputBufferLength = pStack->Parameters.DeviceIoControl.InputBufferLength;
    pNextStack->Parameters.DeviceIoControl.Type3InputBuffer = pStack->Parameters.DeviceIoControl.Type3InputBuffer;
    pNextStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION;
    pNextStack->DeviceObject = pDevExt->NextDeviceObject;
    ntStatus = IoCallDriver(pDevExt->NextDeviceObject, pIrp);
    if (NT_SUCCESS(ntStatus))
    {
        *AcquiredLock = FALSE;//很关键
    }

    return ntStatus;
}



NTSTATUS HumSetIdle(IN PDEVICE_OBJECT pDeviceObject)
{
    NTSTATUS ntStatus;
    PURB pUrb;
    ULONG TypeSize;
    PDEVICE_EXTENSION DeviceExtension;

    DBGPRINT(1, ("HumSetIdle Enter"));
    //RegDebug(L"HumSetIdle start", NULL, ++runtimes_step);
    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);

    if (DeviceExtension) {
        //
        // Allocate buffer
        //

        TypeSize = (USHORT)sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST);

        pUrb = ExAllocatePoolWithTag(NonPagedPoolNx, TypeSize, HIDUSB_TAG);

        if (pUrb) {
            RtlZeroMemory(pUrb, TypeSize);

            if (DeviceExtension->DeviceFlags & DEVICE_FLAGS_HID_1_0_D3_COMPAT_DEVICE) {
                HumBuildClassRequest(pUrb,
                                     URB_FUNCTION_CLASS_ENDPOINT,   // function
                                     0,              // transferFlags
                                     NULL,           // transferBuffer
                                     0,              // transferBufferLength
                                     0x22,           // requestTypeFlags
                                     HID_SET_IDLE,   // request
                                     0,              // value
                                     0,              // index
                                     0);             // reqLength
            }
            else {
                HumBuildClassRequest(pUrb,
                                     URB_FUNCTION_CLASS_INTERFACE,   // function
                                     0,                                  // transferFlags
                                     NULL,                               // transferBuffer
                                     0,                                  // transferBufferLength
                                     0x22,                               // requestTypeFlags
                                     HID_SET_IDLE,                       // request
                                     0,                                  // value
                                     DeviceExtension->pInterfaceInfo->InterfaceNumber,    // index
                                     0);                                 // reqLength
            }

            ntStatus = HumCallUSB(pDeviceObject, pUrb);
            //
            if (NT_SUCCESS(ntStatus)) {
                //RegDebug(L"HumSetIdle HumCallUSB ok", NULL, runtimes_step);
            }
            ExFreePool(pUrb);
        }
        else {
            //RegDebug(L"HumSetIdle STATUS_NO_MEMORY", NULL, runtimes_step);
            ntStatus = STATUS_NO_MEMORY;
        }
    }
    else {
        //RegDebug(L"HumSetIdle STATUS_NOT_FOUND", NULL, runtimes_step);
        ntStatus = STATUS_NOT_FOUND;
    }

    DBGPRINT(1, ("HumSetIdle Exit = %x", ntStatus));
    //RegDebug(L"HumSetIdle end", NULL, runtimes_step);
    return ntStatus;
}


VOID HumSetIdleWorker(PDEVICE_OBJECT pDeviceObject, PVOID Context)
{
    HumSetIdle(pDeviceObject);
    IoFreeWorkItem((PIO_WORKITEM)Context);
}

NTSTATUS
HumGetDescriptorRequest(
    IN PDEVICE_OBJECT pDeviceObject,
    IN USHORT Function,
    IN CHAR DescriptorType,
    IN OUT PVOID* pDescBuffer,
    IN OUT ULONG* pDescBuffLen,
    IN INT TypeSize,
    IN CHAR Index,
    IN USHORT LangID
)
{

    UNREFERENCED_PARAMETER(TypeSize);//不使用该参数，实际就是sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST)常量值
    NTSTATUS ntStatus = STATUS_SUCCESS;
    PURB pUrb;
    BOOLEAN bBufferAllocated = FALSE;//AllocOnBehalf

    DBGPRINT(1, ("HumGetDescriptorRequest Enter"));
    DBGPRINT(1, ("DeviceObject = %x", DeviceObject));
    //
    // Allocate Descriptor buffer
    //
    pUrb = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), HIDUSB_TAG);//TypeSize
    if (pUrb) {
        // Allocate Buffer for Caller if wanted

        if (!*pDescBuffer) {
            *pDescBuffer = ExAllocatePoolWithTag(NonPagedPoolNx, *pDescBuffLen, HIDUSB_TAG);
            bBufferAllocated = TRUE;
        }

        if (!*pDescBuffer) {
            ntStatus = STATUS_NO_MEMORY;
            ExFreePool(pUrb);
            return ntStatus;
        }

        if (!bBufferAllocated) {
            RtlZeroMemory(*pDescBuffer, *pDescBuffLen);
        }
        
        //效果一样
        HumBuildGetDescriptorRequest(pUrb, Function, (USHORT)TypeSize, (CHAR)DescriptorType,(UCHAR)Index, (USHORT)LangID, *pDescBuffer, NULL, *pDescBuffLen, NULL);
        //pUrb->UrbHeader.Function = Function;
        //pUrb->UrbHeader.Length = (USHORT)TypeSize; //TypeSize = sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST);
        //pUrb->UrbControlDescriptorRequest.TransferBufferLength = *pDescBuffLen;
        //pUrb->UrbControlDescriptorRequest.TransferBufferMDL = 0;
        //pUrb->UrbControlDescriptorRequest.TransferBuffer = *pDescBuffer;
        //pUrb->UrbControlDescriptorRequest.DescriptorType = DescriptorType;
        //pUrb->UrbControlDescriptorRequest.Index = Index;
        //pUrb->UrbControlDescriptorRequest.LanguageId = LangID;
        //pUrb->UrbControlDescriptorRequest.UrbLink = 0;

        ntStatus = HumCallUSB(pDeviceObject, pUrb);
        if(!NT_SUCCESS(ntStatus)) {
            //RegDebug(L"HumGetDescriptorRequest HumCallUSB err", NULL, ntStatus);
        }
        else {
        {
            DBGPRINT(1, ("Descriptor = %x, length = %x, status = %x", *pDescBuffer, Urb->UrbControlDescriptorRequest.TransferBufferLength, Urb->UrbHeader.Status));

            if (USBD_SUCCESS(pUrb->UrbHeader.Status)) {
                ntStatus = STATUS_SUCCESS;
                *pDescBuffLen = pUrb->UrbControlDescriptorRequest.TransferBufferLength;
                ExFreePool(pUrb);
                goto Exit;
            }

            ntStatus = STATUS_UNSUCCESSFUL;
            }
        }

        if (bBufferAllocated) {
            ExFreePool(*pDescBuffer);
            *pDescBuffer = NULL;
        }
    }
    else {
        ntStatus = STATUS_NO_MEMORY;
    }

    DBGPRINT(1, ("HumGetDescriptorRequest Exit = %x", ntStatus));


Exit:
    return ntStatus;
}


NTSTATUS HumCallUsbComplete(IN PDEVICE_OBJECT pDeviceObject,
                            IN PIRP pIrp,
                            IN PVOID Context)
{
    UNREFERENCED_PARAMETER(pDeviceObject);
    UNREFERENCED_PARAMETER(pIrp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS HumCallUSB(IN PDEVICE_OBJECT pDeviceObject, IN PURB pUrb)
{
    NTSTATUS              status;
    PIRP                  pIrp;
    IO_STATUS_BLOCK       IoStatusBlock;
    PHID_DEVICE_EXTENSION pDevExt;
    KEVENT                Event;
    PIO_STACK_LOCATION    NextStack;

    pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    pIrp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_SUBMIT_URB,
        pDevExt->NextDeviceObject,
        NULL,
        0,
        NULL,
        0,
        TRUE,
        &Event,
        &IoStatusBlock);

    if (pIrp == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        status = IoSetCompletionRoutineEx(pDeviceObject, pIrp, HumCallUsbComplete, &Event, TRUE, TRUE, TRUE);
        if (NT_SUCCESS(status) == FALSE)
        {
            IofCompleteRequest(pIrp, IO_NO_INCREMENT);
            return status;
        }

        NextStack = IoGetNextIrpStackLocation(pIrp);
        NextStack->Parameters.Others.Argument1 = pUrb;
        status = IofCallDriver(pDevExt->NextDeviceObject, pIrp);
        if (status == STATUS_PENDING)
        {
            /*
             *  Specify a timeout of 5 seconds for this call to complete.
             */
            NTSTATUS StatusWait;
            StatusWait = KeWaitForSingleObject(&Event, Suspended, KernelMode, FALSE, &UrbTimeout);
            if (StatusWait == STATUS_TIMEOUT)
            {
                IoCancelIrp(pIrp);

                StatusWait = KeWaitForSingleObject(&Event, Suspended, KernelMode, FALSE, NULL);
                IoStatusBlock.Status = STATUS_IO_TIMEOUT;
            }
        }
        IofCompleteRequest(pIrp, IO_NO_INCREMENT);
        if (status == STATUS_PENDING)
        {
            status = IoStatusBlock.Status;
        }

    }

    return status;
}


NTSTATUS HumInitDevice(IN PDEVICE_OBJECT pDeviceObject)
{
    NTSTATUS ntStatus;
    PDEVICE_EXTENSION   DeviceExtension;
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    PUSB_CONFIGURATION_DESCRIPTOR pConfigDesc;
    PUSB_INTERFACE_DESCRIPTOR pInterfaceDescriptor;

    ULONG DescriptorLength = 0;

    RegDebug(L"HumInitDevice start", NULL, ++runtimes_step);
    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
    ConfigurationDescriptor = NULL;

    ntStatus = HumGetDeviceDescriptor(pDeviceObject, DeviceExtension);
    if (!NT_SUCCESS(ntStatus)) {
        RegDebug(L"HumInitDevice HumGetDeviceDescriptor err", NULL, ntStatus);
        return ntStatus;
    }

    ntStatus = HumGetConfigDescriptor(pDeviceObject, &ConfigurationDescriptor, &DescriptorLength);
    if (!NT_SUCCESS(ntStatus)) {
        RegDebug(L"HumInitDevice HumGetConfigDescriptor err", NULL, ntStatus);
        return STATUS_UNSUCCESSFUL;
    }

    ntStatus = STATUS_SUCCESS;
    pConfigDesc = ConfigurationDescriptor;

    DeviceExtension->HidDescriptorLength = 0;
    DeviceExtension->PowerFlag = 0;

    pInterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(
        (PUSB_CONFIGURATION_DESCRIPTOR)ConfigurationDescriptor,
        (PVOID)ConfigurationDescriptor,
        -1,
        -1,
        USB_DEVICE_CLASS_HUMAN_INTERFACE,
        -1,
        -1);
    if (pInterfaceDescriptor) { 
        RegDebug(L"HumInitDevice pInterfaceDescriptor=", pInterfaceDescriptor, pInterfaceDescriptor->bLength);

        ConfigurationDescriptor = NULL;
        if (pInterfaceDescriptor->bInterfaceClass == USB_DEVICE_CLASS_HUMAN_INTERFACE) {
            ULONG InterfaceLen = DescriptorLength + pConfigDesc->bLength - pInterfaceDescriptor->bLength;
            //ULONG InterfaceLen = DescriptorLength + pConfigDesc - (UINT_PTR)pInterfaceDescriptor;
            HumParseHidInterface(DeviceExtension, pInterfaceDescriptor, InterfaceLen, &ConfigurationDescriptor);
            if (ConfigurationDescriptor) {
                DeviceExtension->HidDescriptorLength = pConfigDesc->bLength;
                DeviceExtension->PowerFlag = pConfigDesc->MaxPower;
                //新增修正代码
                RtlCopyMemory(&DeviceExtension->UsbConfigDesc, pConfigDesc, sizeof(DeviceExtension->UsbConfigDesc));
                PHID_DESCRIPTOR pHidDesc = (PHID_DESCRIPTOR)ConfigurationDescriptor;
                RtlCopyMemory(&DeviceExtension->HidDescriptor, pHidDesc, sizeof(DeviceExtension->HidDescriptor));//pHidDesc->bLength可变长度可能溢出
                //RtlCopyMemory(&DeviceExtension->HidDescriptor, pConfigDesc + pConfigDesc->bLength, sizeof(DeviceExtension->HidDescriptor));
                //RegDebug(L"HumInitDevice DeviceExtension->HidDescriptor.bDescriptorType", NULL, DeviceExtension->HidDescriptor.bDescriptorType);
                //RegDebug(L"HumInitDevice DeviceExtension->HidDescriptor.bcdHID", NULL, DeviceExtension->HidDescriptor.bcdHID);
                //RegDebug(L"HumInitDevice DeviceExtension->HidDescriptor.bLength", NULL, DeviceExtension->HidDescriptor.bLength);
                //RegDebug(L"HumInitDevice DeviceExtension->HidDescriptor.bCountry", NULL, DeviceExtension->HidDescriptor.bCountry);
                //RegDebug(L"HumInitDevice DeviceExtension->HidDescriptor.bNumDescriptors", NULL, DeviceExtension->HidDescriptor.bNumDescriptors);
                //RegDebug(L"HumInitDevice DeviceExtension->HidDescriptor.DescriptorList[0].bReportType", NULL, DeviceExtension->HidDescriptor.DescriptorList[0].bReportType);
                RegDebug(L"HumInitDevice DeviceExtension->HidDescriptor.DescriptorList[0].wReportLength", NULL, DeviceExtension->HidDescriptor.DescriptorList[0].wReportLength);

                RegDebug(L"HumInitDevice pConfigDesc->bLength", NULL, pConfigDesc->bLength);
                RegDebug(L"HumInitDevice pConfigDesc->MaxPower=", NULL, pConfigDesc->MaxPower);

                RegDebug(L"HumInitDevice pConfigDesc->wTotalLength", NULL, pConfigDesc->wTotalLength);
            }
        }
    }
    RegDebug(L"HumInitDevice HumParseHidInterface ok", NULL, ntStatus);
    ntStatus = HumSelectConfiguration(pDeviceObject, pConfigDesc);
    if (NT_SUCCESS(ntStatus)) {
        RegDebug(L"HumInitDevice HumSelectConfiguration ok", NULL, ntStatus);
        HumSetIdle(pDeviceObject);
        if (pConfigDesc) {
            ExFreePool(pConfigDesc);
        }        
        //goto Exit;
    }


 //Exit:

    RegDebug(L"HumInitDevice end", NULL, runtimes_step);
    return ntStatus;
}


NTSTATUS
HumSelectConfiguration(
    IN PDEVICE_OBJECT pDeviceObject,
    IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor
)
{
    PDEVICE_EXTENSION DeviceExtension;
    NTSTATUS ntStatus;
    PURB pUrb = NULL;
    PUSB_INTERFACE_DESCRIPTOR interfaceDescriptor = NULL;
    USBD_INTERFACE_LIST_ENTRY interfaceList[2];
    PUSBD_INTERFACE_INFORMATION usbInterface=NULL;

    DBGPRINT(1, ("HumSelectConfiguration Entry"));

    UNREFERENCED_PARAMETER(interfaceDescriptor);
    //
    // Get a pointer to the device extension
    //

    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);

    interfaceList[0].InterfaceDescriptor =
        USBD_ParseConfigurationDescriptorEx(
        ConfigurationDescriptor,
        ConfigurationDescriptor,
        -1,
        -1,
        USB_DEVICE_CLASS_HUMAN_INTERFACE,
        -1,
        -1);

    // terminate the list
    interfaceList[1].InterfaceDescriptor =
        NULL;

    if (interfaceList[0].InterfaceDescriptor) {

        pUrb = USBD_CreateConfigurationRequestEx(ConfigurationDescriptor, &interfaceList[0]);

        if (pUrb) {
            ntStatus = HumCallUSB(pDeviceObject, pUrb);
            if (NT_SUCCESS(ntStatus)) {
                DeviceExtension->UsbdConfigurationHandle = pUrb->UrbSelectConfiguration.ConfigurationHandle;
                usbInterface = &pUrb->UrbSelectConfiguration.Interface;

                DBGPRINT(1, ("USBD Interface = 0x%x", usbInterface));
            }
            else {
                DBGWARN(("HumCallUSB failed in HumSelectConfiguration"));
                DeviceExtension->UsbdConfigurationHandle = NULL;
            }

        }
        else {
            DBGWARN(("USBD_CreateConfigurationRequestEx failed in HumSelectConfiguration"));
            ntStatus = STATUS_NO_MEMORY;
        }
    }
    else {
        DBGWARN(("Bad interface descriptor in HumSelectConfiguration"));
        ntStatus = STATUS_INVALID_PARAMETER;
    }

    if (NT_SUCCESS(ntStatus)) {

        DeviceExtension->pInterfaceInfo = ExAllocatePoolWithTag(NonPagedPoolNx, usbInterface->Length, HIDUSB_TAG);

        if (DeviceExtension->pInterfaceInfo) {
            RtlCopyMemory(DeviceExtension->pInterfaceInfo, usbInterface, usbInterface->Length);
            RegDebug(L"HumSelectConfiguration DeviceExtension->pInterfaceInfo=", DeviceExtension->pInterfaceInfo, DeviceExtension->pInterfaceInfo->Length);

           /* RegDebug(L"HumSelectConfiguration pInterfaceInfo->Length=", NULL, DeviceExtension->pInterfaceInfo->Length);
            RegDebug(L"HumSelectConfiguration pInterfaceInfo->Class=", NULL, DeviceExtension->pInterfaceInfo->Class);
            RegDebug(L"HumSelectConfiguration pInterfaceInfo->InterfaceNumber=", NULL, DeviceExtension->pInterfaceInfo->InterfaceNumber);
            RegDebug(L"HumSelectConfiguration pInterfaceInfo->NumberOfPipes=", NULL, DeviceExtension->pInterfaceInfo->NumberOfPipes);
            RegDebug(L"HumSelectConfiguration pInterfaceInfo->Protocol=", NULL, DeviceExtension->pInterfaceInfo->Protocol);
            RegDebug(L"HumSelectConfiguration DeviceExtension->pInterfaceInfo->SubClass=", NULL, DeviceExtension->pInterfaceInfo->SubClass);

            RegDebug(L"HumSelectConfiguration pInterfaceInfo->Pipes[0].EndpointAddress=", NULL, DeviceExtension->pInterfaceInfo->Pipes[0].EndpointAddress);
            RegDebug(L"HumSelectConfiguration DpInterfaceInfo->Pipes[0].MaximumPacketSize=", NULL, DeviceExtension->pInterfaceInfo->Pipes[0].MaximumPacketSize);
            RegDebug(L"HumSelectConfiguration pInterfaceInfo->Pipes[0].MaximumTransferSize=", NULL, DeviceExtension->pInterfaceInfo->Pipes[0].MaximumTransferSize);
            RegDebug(L"HumSelectConfiguration pInterfaceInfo->Pipes[0].PipeType=", NULL, DeviceExtension->pInterfaceInfo->Pipes[0].PipeType);
            RegDebug(L"HumSelectConfiguration pInterfaceInfo->Pipes[0].Interval=", NULL, DeviceExtension->pInterfaceInfo->Pipes[0].Interval);*/

        }
    }

    if (pUrb) {
        ExFreePool(pUrb);
    }

    DBGPRINT(1, ("HumSelectConfiguration Exit = %x", ntStatus));

    return ntStatus;
}


NTSTATUS
HumParseHidInterface(
    IN  PDEVICE_EXTENSION DeviceExtension,
    IN  PUSB_INTERFACE_DESCRIPTOR InterfaceDesc,
    IN  ULONG InterfaceLength,
    OUT PUSB_CONFIGURATION_DESCRIPTOR* pUsbConfigDesc
)
{
    UNREFERENCED_PARAMETER(InterfaceLength);

    NTSTATUS ntStatus = STATUS_SUCCESS;
    ULONG iEndpointIndex = 0;
    PUSB_ENDPOINT_DESCRIPTOR EndpointDesc;
    PUSB_COMMON_DESCRIPTOR CommonDesc;

    LONG                      Remain;
    UINT8                     Length;
    BOOLEAN                   bEndpointChecked = FALSE;

    DBGPRINT(1, ("HumParseHidInterface Entry"));
    *pUsbConfigDesc = NULL;

    if (InterfaceLength < sizeof(USB_INTERFACE_DESCRIPTOR)) {

        DBGWARN(("InterfaceLength (%d) is invalid", InterfaceLength));
        goto Bail;
    }

    if (InterfaceDesc->bLength < sizeof(USB_INTERFACE_DESCRIPTOR)) {

        DBGWARN(("Interface->bLength (%d) is invalid", InterfaceDesc->bLength));
        goto Bail;
    }

    Remain = InterfaceLength - InterfaceDesc->bLength;
    if (Remain < 2) {

        DBGWARN(("Remain (%d) is invalid", Remain));
        goto Bail;
    }
    //
    // For HID 1.0 draft 4 compliance, the next descriptor is HID.  However, for earlier
    // drafts, endpoints come first and then HID.  We're trying to support both.
    //

    DeviceExtension->DeviceFlags &= ~DEVICE_FLAGS_HID_1_0_D3_COMPAT_DEVICE;

    //
    // What draft of HID 1.0 are we looking at?
    //

    CommonDesc = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)InterfaceDesc + InterfaceDesc->bLength);

    if (CommonDesc->bLength < sizeof(USB_COMMON_DESCRIPTOR)) {
        DBGWARN(("Descriptor->bLength (%d) is invalid", CommonDesc->bLength));
        goto Bail;
    }

    if (CommonDesc->bDescriptorType == HID_HID_DESCRIPTOR_TYPE) {
        if (CommonDesc->bLength != 0x09) {// length of HID descriptor)
            goto Bail;
        }

        *pUsbConfigDesc = (PUSB_CONFIGURATION_DESCRIPTOR)CommonDesc;
        Remain -= CommonDesc->bLength;
        if (Remain < 0) {
            DBGWARN(("HID descriptor length (%d) is invalid!", CommonDesc->bLength));
            *pUsbConfigDesc = NULL;
            goto Bail;
        }

        CommonDesc = (PUSB_COMMON_DESCRIPTOR)((ULONG_PTR)CommonDesc + CommonDesc->bLength);
    }
    else {
        DeviceExtension->DeviceFlags |= DEVICE_FLAGS_HID_1_0_D3_COMPAT_DEVICE;
    }

    iEndpointIndex = 0;
    EndpointDesc = (PUSB_ENDPOINT_DESCRIPTOR)CommonDesc;
    bEndpointChecked = TRUE;
    if (InterfaceDesc->bNumEndpoints != 0)
    {
        while(Remain>=2)
        {
            if (EndpointDesc->bDescriptorType == 5)
            {
                if (EndpointDesc->bLength != sizeof(USB_ENDPOINT_DESCRIPTOR))
                {
                    goto Bail;
                }

                Length = EndpointDesc->bLength;
                iEndpointIndex++;
            }
            else
            {
                if (EndpointDesc->bDescriptorType == 4)
                {
                    goto Bail;
                }

                Length = EndpointDesc->bLength;
                if (!Length) {
                    goto Bail;
                }
            }

            Remain -= Length;
            if (Remain < 0)
            {
                goto Bail;
            }

            EndpointDesc = EndpointDesc + Length;
            if (iEndpointIndex == InterfaceDesc->bNumEndpoints)
            {
                break;
            }
        }
    }

    // Walk endpoints
    //

    CommonDesc = (PUSB_COMMON_DESCRIPTOR)EndpointDesc;
    if ((DeviceExtension->DeviceFlags & DEVICE_FLAGS_HID_1_0_D3_COMPAT_DEVICE) == 0) {
        if (*pUsbConfigDesc == NULL) {
            // We did not find a HID descriptor in this interface!
            DBGWARN(("Failed to find a valid HID descriptor in interface!"));
        }
    }

    if(Remain >= 2){
        if (CommonDesc->bDescriptorType != HID_HID_DESCRIPTOR_TYPE) {
            if (CommonDesc->bLength != 0x9) {// length of HID descriptor)
                if (Remain < CommonDesc->bLength) {
                    if (Remain < 0x9) { //sizeof(USB_INTERFACE_DESCRIPTOR)??
                        *pUsbConfigDesc = NULL;
                    }

                    goto Bail;
                }
                goto Bail;
            }
        }

        *pUsbConfigDesc = (PUSB_CONFIGURATION_DESCRIPTOR)CommonDesc;
        if (Remain < CommonDesc->bLength) {
            if (Remain < 0x9) { //sizeof(USB_INTERFACE_DESCRIPTOR)
                *pUsbConfigDesc = NULL;
            }

            goto Bail;
        }

        goto Bail;
    }


Bail:
    if (*pUsbConfigDesc == NULL) {
        //
        // We did not find a HID descriptor in this interface!
        DBGWARN(("Failed to find a valid HID descriptor in interface!"));

        ntStatus = STATUS_UNSUCCESSFUL;
    }

    if (bEndpointChecked) {
        if (iEndpointIndex != InterfaceDesc->bNumEndpoints) {
            ntStatus = STATUS_UNSUCCESSFUL;
        }

    }

    DBGPRINT(1, ("HumParseHidInterface Exit = 0x%x", ntStatus));

    return ntStatus;
}


NTSTATUS
HumGetConfigDescriptor(
    IN PDEVICE_OBJECT pDeviceObject,
    OUT PUSB_CONFIGURATION_DESCRIPTOR* ppConfigurationDesc,
    OUT PULONG pConfigurationDescLength
)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG                         ConfigDescriptorLength;
    ULONG                         TotalLength;

    PUSB_CONFIGURATION_DESCRIPTOR pConfigDesc = NULL;

    ConfigDescriptorLength = sizeof(USB_CONFIGURATION_DESCRIPTOR);

    //
    // Just get the base config descriptor, so that we can figure out the size,
    // then allocate enough space for the entire descriptor.
    //
    status = HumGetDescriptorRequest(pDeviceObject,
                                       URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                                       USB_CONFIGURATION_DESCRIPTOR_TYPE,
                                       (PVOID*)&pConfigDesc,
                                       &ConfigDescriptorLength,
                                       sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),//0//实际不使用该参数
                                       0,
                                       0);

    if (!NT_SUCCESS(status)) {
    }
    else {
        if (ConfigDescriptorLength < 9)
        {
            return STATUS_DEVICE_DATA_ERROR;
        }

        TotalLength = pConfigDesc->wTotalLength;
        ExFreePool(pConfigDesc);
        if (!TotalLength)
        {
            return STATUS_DEVICE_DATA_ERROR;
        }

        pConfigDesc = NULL;
        ConfigDescriptorLength = TotalLength;
        status = HumGetDescriptorRequest(
            pDeviceObject,
            URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
            USB_CONFIGURATION_DESCRIPTOR_TYPE,
            &pConfigDesc,
            &ConfigDescriptorLength,
            0,
            0,
            0);

        if (NT_SUCCESS(status) == FALSE)
        {
            goto Exit;
        }


        if (ConfigDescriptorLength < 9)
        {
            if (pConfigDesc) {
                ExFreePool(pConfigDesc);
            }

            return STATUS_DEVICE_DATA_ERROR;
        }

        if (pConfigDesc->wTotalLength > TotalLength)
        {
            pConfigDesc->wTotalLength = (USHORT)TotalLength;
        }

        if (pConfigDesc->bLength < 9)
        {
            pConfigDesc->bLength = 9;
        }

    }

Exit:

    *ppConfigurationDesc = pConfigDesc;
    *pConfigurationDescLength = ConfigDescriptorLength;

    return status;
}


NTSTATUS
HumGetDeviceDescriptor(
    IN PDEVICE_OBJECT    pDeviceObject,
    IN PDEVICE_EXTENSION DeviceExtension
)
{
    NTSTATUS ntStatus = STATUS_SUCCESS;
    ULONG DescriptorLength = sizeof(USB_DEVICE_DESCRIPTOR);

    DBGPRINT(1, ("HumGetDeviceDescriptor Entry"));

    //
    // Get config descriptor
    //

    ntStatus = HumGetDescriptorRequest(
        pDeviceObject,
        URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
        USB_DEVICE_DESCRIPTOR_TYPE,
        &DeviceExtension->pUsbDeviceDescriptor,
        &DescriptorLength,
        sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
        0,
        0);

    if (NT_SUCCESS(ntStatus)) {
        RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor=", DeviceExtension->pUsbDeviceDescriptor, DescriptorLength);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->bLength=", NULL, DeviceExtension->pUsbDeviceDescriptor->bLength);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->bDescriptorType=", NULL, DeviceExtension->pUsbDeviceDescriptor->bDescriptorType);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->bDeviceClass=", NULL, DeviceExtension->pUsbDeviceDescriptor->bDeviceClass);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->bDeviceSubClass=", NULL, DeviceExtension->pUsbDeviceDescriptor->bDeviceSubClass);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->bDeviceProtocol=", NULL, DeviceExtension->pUsbDeviceDescriptor->bDeviceProtocol);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->idVendor=", NULL, DeviceExtension->pUsbDeviceDescriptor->idVendor);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->idProduct=", NULL, DeviceExtension->pUsbDeviceDescriptor->idProduct);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->bcdDevice=", NULL, DeviceExtension->pUsbDeviceDescriptor->bcdDevice);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->iManufacturer=", NULL, DeviceExtension->pUsbDeviceDescriptor->iManufacturer);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->iProduct=", NULL, DeviceExtension->pUsbDeviceDescriptor->iProduct);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->iSerialNumber=", NULL, DeviceExtension->pUsbDeviceDescriptor->iSerialNumber);
        //RegDebug(L"HumGetDescriptorRequest pUsbDeviceDescriptor->bNumConfigurations=", NULL, DeviceExtension->pUsbDeviceDescriptor->bNumConfigurations);
        

        // Dump device descriptor
        //
        DBGPRINT(2, ("Device->bLength              = 0x%x", DeviceExtension->pUsbDeviceDescriptor->bLength));
        DBGPRINT(2, ("Device->bDescriptorType      = 0x%x", DeviceExtension->pUsbDeviceDescriptor->bDescriptorType));
        DBGPRINT(2, ("Device->bDeviceClass         = 0x%x", DeviceExtension->pUsbDeviceDescriptor->bDeviceClass));
        DBGPRINT(2, ("Device->bDeviceSubClass      = 0x%x", DeviceExtension->pUsbDeviceDescriptor->bDeviceSubClass));
        DBGPRINT(2, ("Device->bDeviceProtocol      = 0x%x", DeviceExtension->pUsbDeviceDescriptor->bDeviceProtocol));
        DBGPRINT(2, ("Device->idVendor             = 0x%x", DeviceExtension->pUsbDeviceDescriptor->idVendor));
        DBGPRINT(2, ("Device->idProduct            = 0x%x", DeviceExtension->pUsbDeviceDescriptor->idProduct));
        DBGPRINT(2, ("Device->bcdDevice            = 0x%x", DeviceExtension->pUsbDeviceDescriptor->bcdDevice));
    }
    else {
        DBGWARN(("HumGetDescriptorRequest failed w/ %xh in HumGetDeviceDescriptor", (ULONG)ntStatus));
    }

    DBGPRINT(1, ("HumGetDeviceDescriptor Exit = 0x%x", ntStatus));

    return ntStatus;
}



NTSTATUS DriverEntry(IN PDRIVER_OBJECT pDriverObject, IN PUNICODE_STRING registryPath)
{
    NTSTATUS ntStatus = STATUS_SUCCESS;
    HID_MINIDRIVER_REGISTRATION hidMinidriverRegistration;

    RegDebug(L"DriverEntry Enter", NULL, 0);
    DBGPRINT(1, ("DriverEntry Enter"));
    DBGPRINT(1, ("DriverObject (%lx)", DriverObject));

    pDriverObject->MajorFunction[IRP_MJ_CREATE] = HumCreateClose;
    pDriverObject->MajorFunction[IRP_MJ_CLOSE] = HumCreateClose;
    pDriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = HumInternalIoctl;
    pDriverObject->MajorFunction[IRP_MJ_PNP] = HumPnP;
    pDriverObject->MajorFunction[IRP_MJ_POWER] = HumPower;
    pDriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = HumSystemControl;
    pDriverObject->DriverExtension->AddDevice = HumAddDevice;
    pDriverObject->DriverUnload = HumUnload;

    // Register USB layer with HID.SYS module
    hidMinidriverRegistration.Revision = HID_REVISION;
    hidMinidriverRegistration.DriverObject = pDriverObject;
    hidMinidriverRegistration.RegistryPath = registryPath;
    hidMinidriverRegistration.DeviceExtensionSize = sizeof(DEVICE_EXTENSION);

    /*
     *  HIDUSB is a minidriver for USB devices, which do not need to be polled.
     */
    hidMinidriverRegistration.DevicesArePolled = FALSE;

    DBGPRINT(1, ("DeviceExtensionSize = %x", hidMinidriverRegistration.DeviceExtensionSize));

    DBGPRINT(1, ("Registering with HID.SYS"));

    ntStatus = HidRegisterMinidriver(&hidMinidriverRegistration);

    //KeInitializeSpinLock(&resetWorkItemsListSpinLock);

    DBGPRINT(1, ("DriverEntry Exit = %x", ntStatus));

    runtimes_step = 0;
    runtimes_ioctl = 0;
    runtimes_readinput = 0;
    RegDebug(L"DriverEntry Exit", NULL, 0);
    return ntStatus;
}

NTSTATUS HumAddDevice(IN PDRIVER_OBJECT pDriverObject, IN PDEVICE_OBJECT pFunctionalDeviceObject)
{
    UNREFERENCED_PARAMETER(pDriverObject);

    NTSTATUS                ntStatus = STATUS_SUCCESS;
    PDEVICE_EXTENSION       pMiniDevExt;

    RegDebug(L"HumAddDevice Entry", NULL, ++runtimes_step);
    DBGPRINT(1, ("HumAddDevice Entry"));

    pMiniDevExt = GET_MINIDRIVER_DEVICE_EXTENSION(pFunctionalDeviceObject);

    pMiniDevExt->DeviceFlags = 0;

    pMiniDevExt->nPendingRequestsCount = 0;
    KeInitializeEvent(&pMiniDevExt->AllRequestsCompleteEvent, NotificationEvent, FALSE);

    pMiniDevExt->pResetWorkItem = NULL;
    pMiniDevExt->PnpState = DEVICE_STATE_NONE;
    pMiniDevExt->pFDO = pFunctionalDeviceObject;
    KeInitializeSpinLock(&pMiniDevExt->DeviceResetNotificationSpinLock);

    pMiniDevExt->pDeviceResetNotificationIrp = NULL;
    IoInitializeRemoveLockEx(&pMiniDevExt->RemoveLock, HIDUSB_TAG, 2, 0, sizeof(pMiniDevExt->RemoveLock));

    pMiniDevExt->pReportDesc = NULL;//新增
    pMiniDevExt->ReportDescLength = 0;//新增
    DBGPRINT(1, ("HumAddDevice  = %x", ntStatus));
    RegDebug(L"HumAddDevice Exit", NULL, runtimes_step);
    return ntStatus;
}


NTSTATUS HumCreateClose(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    PIO_STACK_LOCATION   IrpStack;
    NTSTATUS             ntStatus = STATUS_SUCCESS;

    DBGPRINT(1, ("HumCreateClose Enter"));

    // Get a pointer to the current location in the Irp.
    //
    IrpStack = IoGetCurrentIrpStackLocation(pIrp);

    switch (IrpStack->MajorFunction)
    {
        case IRP_MJ_CREATE:
            DBGPRINT(1, ("IRP_MJ_CREATE"));
            pIrp->IoStatus.Information = 0;
            break;

        case IRP_MJ_CLOSE:
            DBGPRINT(1, ("IRP_MJ_CLOSE"));
            pIrp->IoStatus.Information = 0;
            break;

        default:
            DBGPRINT(1, ("Invalid CreateClose Parameter"));
            ntStatus = STATUS_INVALID_PARAMETER;
            break;
    }

    //
    // Save Status for return and complete Irp
    //

    pIrp->IoStatus.Status = ntStatus;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);

    DBGPRINT(1, ("HumCreateClose Exit = %x", ntStatus));

    return ntStatus;
}



VOID HumUnload(IN PDRIVER_OBJECT DriverObject
)
{
    UNREFERENCED_PARAMETER(DriverObject);

    DBGPRINT(1, ("HumUnload Enter"));

    DBGPRINT(1, ("Unloading DriverObject = %x", DriverObject));

    DBGPRINT(1, ("Unloading Exit = VOID"));

    RegDebug(L"HumUnload Exit", NULL, ++runtimes_step);
}





LONG HumDecrementPendingRequestCount(IN PDEVICE_EXTENSION DeviceExtension)
{
    LONG PendingCount;

    PendingCount = InterlockedExchangeAdd(&DeviceExtension->nPendingRequestsCount, 0xFFFFFFFF);
    if (PendingCount < 1) {
        PendingCount = KeSetEvent(&DeviceExtension->AllRequestsCompleteEvent, IO_NO_INCREMENT, FALSE);
    }

    return PendingCount;
}


NTSTATUS HumGetPortStatus(IN PDEVICE_OBJECT pDeviceObject, IN PULONG pPortStatus)
{
    NTSTATUS ntStatus;
    PIRP pIrp;
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;
    PIO_STACK_LOCATION nextStack;

    *pPortStatus = 0;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    pIrp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_GET_PORT_STATUS,
        GET_NEXT_DEVICE_OBJECT(pDeviceObject),
        NULL,
        0,
        NULL,
        0,
        TRUE, /* INTERNAL */
        &event,
        &ioStatus);

    if (!pIrp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Call the class driver to perform the operation.  If the returned status
    // is PENDING, wait for the request to complete.

    nextStack = IoGetNextIrpStackLocation(pIrp);
    nextStack->Parameters.Others.Argument1 = pPortStatus;

    ntStatus = IoCallDriver(GET_NEXT_DEVICE_OBJECT(pDeviceObject), pIrp);
    if (ntStatus == STATUS_PENDING) {
        ntStatus = KeWaitForSingleObject(
            &event,
            Suspended,
            KernelMode,
            FALSE,
            NULL);

        ntStatus = ioStatus.Status;
    }

    return ntStatus;
}

NTSTATUS HumGetSetReport(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, BOOLEAN* NeedsCompletion)
{
    NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;
    PDEVICE_EXTENSION DeviceExtension;
    PIO_STACK_LOCATION currentIrpStack, nextIrpStack;
    PHID_XFER_PACKET reportPacket;

    ULONG transferFlags=0;
    UCHAR request=0;
    USHORT value=0;

    PIO_STACK_LOCATION  irpSp;

    

    irpSp = IoGetCurrentIrpStackLocation(pIrp);

    switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_HID_GET_INPUT_REPORT:
            transferFlags = USBD_TRANSFER_DIRECTION_IN;
            request = 0x01;
            value = 0x0100;
            break;
        case IOCTL_HID_SET_OUTPUT_REPORT:
            transferFlags = USBD_TRANSFER_DIRECTION_OUT;
            request = 0x09;
            value = 0x0200;
            break;
        case IOCTL_HID_SET_FEATURE:
            transferFlags = USBD_TRANSFER_DIRECTION_OUT;
            request = 0x09;
            value = 0x0300;
            break;
        case IOCTL_HID_GET_FEATURE:
            transferFlags = USBD_TRANSFER_DIRECTION_IN;
            request = 0x01;
            value = 0x0300;
            break;
        default:
            RegDebug(L"HumGetSetReport STATUS_NOT_SUPPORTED", NULL, runtimes_ioctl);
            return STATUS_NOT_SUPPORTED;
            break;
    }

    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
    currentIrpStack = IoGetCurrentIrpStackLocation(pIrp);
    nextIrpStack = IoGetNextIrpStackLocation(pIrp);

    reportPacket = pIrp->UserBuffer;
    if (reportPacket && reportPacket->reportBuffer && reportPacket->reportBufferLen) {
        PURB pUrb = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(URB), HIDUSB_TAG);

        if (pUrb) {

            RtlZeroMemory(pUrb, sizeof(URB));

            value += reportPacket->reportId;

            /*
             *   A control operation consists of 3 stages: setup, data, and status.
             *   In the setup stage the device receives an 8-byte frame comprised of
             *   the following fields of a _URB_CONTROL_VENDOR_OR_CLASS_REQUEST structure:
             *   See section 7.2 in the USB HID specification for how to fill out these fields.
             *
             *      UCHAR RequestTypeReservedBits;
             *      UCHAR Request;
             *      USHORT Value;
             *      USHORT Index;
             *
             */
            if (DeviceExtension->DeviceFlags & DEVICE_FLAGS_HID_1_0_D3_COMPAT_DEVICE) {
                HumBuildClassRequest(
                    pUrb,
                    URB_FUNCTION_CLASS_ENDPOINT,
                    transferFlags,
                    reportPacket->reportBuffer,
                    reportPacket->reportBufferLen,
                    0x22, // requestType= Set_Report Request,
                    request,
                    value, // value= reportType 'report' &reportId,
                    1,                  // index= endpoint 1,
                    hidWritePacket->reportBufferLen    // reqLength (not used)
                );
            }
            else {
                HumBuildClassRequest(
                    pUrb,
                    URB_FUNCTION_CLASS_INTERFACE,
                    transferFlags,
                    reportPacket->reportBuffer,
                    reportPacket->reportBufferLen,
                    0x22, // requestType= Set_Report Request,
                    request,
                    value, // value= reportType 'report' &reportId,
                    DeviceExtension->pInterfaceInfo->InterfaceNumber, // index= interface,
                    hidWritePacket->reportBufferLen    // reqLength (not used)
                );
            }

            IoSetCompletionRoutine(pIrp, HumGetSetReportCompletion, pUrb, TRUE, TRUE, TRUE);

            nextIrpStack->Parameters.Others.Argument1 = pUrb;
            nextIrpStack->MajorFunction = currentIrpStack->MajorFunction;
            nextIrpStack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
            nextIrpStack->DeviceObject = GET_NEXT_DEVICE_OBJECT(pDeviceObject);

            //
            // We need to keep track of the number of pending requests
            // so that we can make sure they're all cancelled properly during
            // processing of a stop device request.
            //

            if (NT_SUCCESS(HumIncrementPendingRequestCount(DeviceExtension))) {

                ntStatus = IoCallDriver(GET_NEXT_DEVICE_OBJECT(pDeviceObject), pIrp);
                RegDebug(L"HumGetSetReport IoCallDriver ntStatus", NULL, ntStatus);
                *NeedsCompletion = FALSE;

            }
            else {
                ExFreePool(pUrb);

                ntStatus = STATUS_NO_SUCH_DEVICE;
                RegDebug(L"HumGetSetReport STATUS_NO_SUCH_DEVICE", NULL, ntStatus);
            }

        }
        else {
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
            RegDebug(L"HumGetSetReport STATUS_INSUFFICIENT_RESOURCES", NULL, ntStatus);
        }
    }
    else {
        ntStatus = STATUS_DATA_ERROR;
        RegDebug(L"HumGetSetReport STATUS_DATA_ERROR", NULL, ntStatus);
    }

    return ntStatus;
}


NTSTATUS HumGetSetReportCompletion(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, IN PVOID Context)
{
    PURB pUrb = (PURB)Context;
    PDEVICE_EXTENSION pMiniDevExt;

    pMiniDevExt = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);

    if (NT_SUCCESS(pIrp->IoStatus.Status)) {
        /*
         *  Record the number of bytes written.
         */
        pIrp->IoStatus.Information = (ULONG)pUrb->UrbBulkOrInterruptTransfer.TransferBufferLength;
    }

    ExFreePool(pUrb);

    /*
     *  If the lower driver returned PENDING, mark our stack location as
     *  pending also. This prevents the IRP's thread from being freed if
     *  the client's call returns pending.
     */
    if (pIrp->PendingReturned) {
        IoMarkIrpPending(pIrp);
    }

    /*
     *  Balance the increment we did when we issued this IRP.
     */
    HumDecrementPendingRequestCount(pMiniDevExt);
    IoReleaseRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, sizeof(pMiniDevExt->RemoveLock));
    return STATUS_SUCCESS;
}

NTSTATUS HumIncrementPendingRequestCount(IN PDEVICE_EXTENSION DeviceExtension)
{
    InterlockedIncrement(&DeviceExtension->nPendingRequestsCount);
    if (DeviceExtension->PnpState == DEVICE_STATE_RUNNING || DeviceExtension->PnpState == DEVICE_STATE_STARTING)
    {
        return STATUS_SUCCESS;
    }
    HumDecrementPendingRequestCount(DeviceExtension);
    return STATUS_NO_SUCH_DEVICE;
}


NTSTATUS HumResetInterruptPipe(IN PDEVICE_OBJECT pDeviceObject)
{
    NTSTATUS ntStatus;
    PURB pUrb;
    PDEVICE_EXTENSION DeviceExtension;
    PUSBD_PIPE_INFORMATION pipeInfo;

    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
    USHORT UrbLen = (USHORT)sizeof(struct _URB_PIPE_REQUEST);
    pUrb = ExAllocatePoolWithTag(NonPagedPoolNx, UrbLen, HIDUSB_TAG);
    if (pUrb == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pUrb->UrbHeader.Length = UrbLen;
    pUrb->UrbHeader.Function = URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL;
    pipeInfo = GetInterruptInputPipeForDevice(DeviceExtension);
    if (pipeInfo) {
        pUrb->UrbPipeRequest.PipeHandle = pipeInfo->PipeHandle;

        ntStatus = HumCallUSB(pDeviceObject, pUrb);
    }
    else {
        //
        // This device doesn't have an interrupt IN pipe. 
        // Odd, but possible. I.e. USB monitor
        //
        ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    }

    ExFreePool(pUrb);
    return ntStatus;
}



NTSTATUS HumResetParentPort(IN PDEVICE_OBJECT pDeviceObject)
{
    NTSTATUS ntStatus;
    PIRP pIrp;
    KEVENT event;
    IO_STATUS_BLOCK ioStatus;

    //
    // issue a synchronous request
    //

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    pIrp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_USB_RESET_PORT,
        GET_NEXT_DEVICE_OBJECT(pDeviceObject),
        NULL,
        0,
        NULL,
        0,
        TRUE, /* INTERNAL */
        &event,
        &ioStatus);

    if (!pIrp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    //
    // Call the class driver to perform the operation.  If the returned status
    // is PENDING, wait for the request to complete.
    //

    ntStatus = IoCallDriver(GET_NEXT_DEVICE_OBJECT(pDeviceObject), pIrp);
    if (ntStatus == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Suspended, KernelMode, FALSE, NULL);
        ntStatus = ioStatus.Status;
    }

    return ntStatus;
}


NTSTATUS HumGetStringDescriptor(IN PDEVICE_OBJECT pDeviceObject,
                                IN PIRP pIrp)
{
    NTSTATUS ntStatus = STATUS_PENDING;
    PDEVICE_EXTENSION DeviceExtension;
    PIO_STACK_LOCATION IrpStack;
    PVOID buffer;
    ULONG bufferSize;
    BOOLEAN isIndexedString;
    ULONG                   IoControlCode;

    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);

    IrpStack = IoGetCurrentIrpStackLocation(pIrp);
    IoControlCode = IrpStack->Parameters.DeviceIoControl.IoControlCode;
    if (IoControlCode == IOCTL_HID_GET_STRING)
    {
        buffer = pIrp->UserBuffer;
        isIndexedString = FALSE;
    }
    else
    {
        if (IoControlCode != IOCTL_HID_GET_INDEXED_STRING)
        {
            RegDebug(L"HumGetStringDescriptor STATUS_INVALID_USER_BUFFER", NULL, runtimes_ioctl);
            return STATUS_INVALID_USER_BUFFER;
        }
    }

    buffer = HumGetSystemAddressForMdlSafe(pIrp->MdlAddress);
    if (!buffer) {
        RegDebug(L"HumGetStringDescriptor HumGetSystemAddressForMdlSafe err", NULL, runtimes_ioctl);
        return STATUS_INVALID_USER_BUFFER;
    }

    isIndexedString = TRUE;
    bufferSize = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;
    if (bufferSize < 2) {
        RegDebug(L"HumGetStringDescriptor IrpStack->Parameters.DeviceIoControl.OutputBufferLength err", NULL, runtimes_ioctl);
        return STATUS_INVALID_USER_BUFFER;
    }


    ULONG bufferlength = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;
    RegDebug(L"HidGetString: bufferlength=", NULL, bufferlength);
    USHORT stringSizeCb = sizeof(L"9999");
    PWSTR string = L"9999";
    int i = -1;
    do {
        ++i;
    } while (string[i]);

    stringSizeCb = (USHORT)(2 * i + 2);
    RtlMoveMemory(pIrp->UserBuffer, string, stringSizeCb);
    pIrp->IoStatus.Information = stringSizeCb;
    if (1)
        return STATUS_SUCCESS;


    /*
        *  String id and language id are in Type3InputBuffer field
        *  of IRP stack location.
        *
        *  Note: the string ID should be identical to the string's
        *        field offset given in Chapter 9 of the USB spec.
        */
    ULONG Type3InputBufferValue = PtrToUlong(IrpStack->Parameters.DeviceIoControl.Type3InputBuffer);
    
    USHORT languageId = Type3InputBufferValue >> 16;// HIWORD(Type3InputBufferValue);// Type3InputBufferValue>> 16;
    SHORT stringIndex = 0;
    RegDebug(L"HumGetStringDescriptor languageId=", NULL, languageId);

    if (isIndexedString) {
        goto NextStep;
    }

    USHORT wStrID = Type3InputBufferValue & 0xFFFF; //LOWORD(Type3InputBufferValue);//    ULONG stringId = Type3InputBufferValue & 0xFFFF;//GetStrCtlCode
    RegDebug(L"HumGetStringDescriptor wStrID=", NULL, wStrID);
    switch (wStrID) {
        case HID_STRING_ID_IMANUFACTURER:
            stringIndex = DeviceExtension->pUsbDeviceDescriptor->iManufacturer;
            if (!stringIndex) {
                return STATUS_INVALID_PARAMETER;
            }
            break;
        case HID_STRING_ID_IPRODUCT:
            stringIndex = DeviceExtension->pUsbDeviceDescriptor->iProduct;
            if (!stringIndex) {
                return STATUS_INVALID_PARAMETER;
            }
            break;
        case HID_STRING_ID_ISERIALNUMBER:
            stringIndex = DeviceExtension->pUsbDeviceDescriptor->iSerialNumber;
            if (!stringIndex) {
                return STATUS_INVALID_PARAMETER;
            }
            break;
        default:
            return STATUS_INVALID_PARAMETER;

    }

NextStep:
    {
    }
        PUSB_STRING_DESCRIPTOR  pStrDesc;
        ULONG                   DescBuffLen;

        /*
            *  USB descriptors begin with an extra two bytes for length and type.
            *  So we need to allocate a slightly larger buffer.
            */
        DescBuffLen = bufferSize + 2;
        pStrDesc = (PUSB_STRING_DESCRIPTOR)ExAllocatePoolWithTag(NonPagedPoolNx, DescBuffLen, HIDUSB_TAG);
        if (pStrDesc) 
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        } 
        ntStatus = HumGetDescriptorRequest(pDeviceObject,
                                            URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                                            USB_STRING_DESCRIPTOR_TYPE,
                                            &pStrDesc,
                                            &DescBuffLen,
                                            sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                            (CHAR)stringIndex,
                                            languageId); // LanguageID,

        if (NT_SUCCESS(ntStatus)) {
            /*
                *  USB descriptors always begin with two bytes for the length
                *  and type.  Remove these.
                */
            ULONG descLen = pStrDesc->bLength - 2;

            if (descLen > DescBuffLen) {
                descLen = DescBuffLen;
            }
            descLen &= 0xFFFFFFFE;
            if (descLen > bufferSize - 2)
            {
                ntStatus = STATUS_INVALID_BUFFER_SIZE;
            }
            else {
                RtlCopyMemory(buffer, &pStrDesc->bString, descLen);

                PWCHAR p = (PWCHAR)((PCHAR)buffer + descLen);
                *p = UNICODE_NULL;
                pIrp->IoStatus.Information = (descLen + 2);
            }

        }

    ExFreePool(pStrDesc);
    return ntStatus;
}


NTSTATUS HumPnP(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_EXTENSION           pMiniDevExt;
    UCHAR                       MinorFunction;
    PUSBD_INTERFACE_INFORMATION pInterfaceInfo;
    PIO_STACK_LOCATION          pStack;
    KEVENT                      Event;
    PHID_DEVICE_EXTENSION       pDevExt;
    ULONG                       PrevPnpState;

    pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
    pMiniDevExt = (PDEVICE_EXTENSION)pDevExt->MiniDeviceExtension;
    pStack = IoGetCurrentIrpStackLocation(pIrp);
    status = IoAcquireRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, __FILE__, __LINE__, sizeof(pMiniDevExt->RemoveLock));
    if (NT_SUCCESS(status) == FALSE)
    {
        pIrp->IoStatus.Status = status;
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
        return status;
    }

    MinorFunction = pStack->MinorFunction;

    switch (MinorFunction) {

        case IRP_MN_START_DEVICE:
            PrevPnpState = pMiniDevExt->PnpState;
            pMiniDevExt->PnpState = DEVICE_STATE_STARTING;
            KeResetEvent(&pMiniDevExt->AllRequestsCompleteEvent);
            if ((PrevPnpState == DEVICE_STATE_STOPPING) ||
                (PrevPnpState == DEVICE_STATE_STOPPED) ||
                (PrevPnpState == DEVICE_STATE_REMOVING))
            {

                HumIncrementPendingRequestCount(pMiniDevExt);
            }
            pMiniDevExt->pInterfaceInfo = NULL;
            break;

        case IRP_MN_STOP_DEVICE:
            if (pMiniDevExt->PnpState == DEVICE_STATE_RUNNING) {
                status = HumStopDevice(pDeviceObject);
                if (!NT_SUCCESS(status)) {
                    goto ExitUnlock;
                }
            }
            break;

        case IRP_MN_REMOVE_DEVICE:
            return HumRemoveDevice(pDeviceObject, pIrp);
            break;
        case IRP_MN_QUERY_PNP_DEVICE_STATE:
            if (pMiniDevExt->PnpState == DEVICE_STATE_START_FAILED)
            {
                pIrp->IoStatus.Information |= PNP_DEVICE_FAILED;
            }
            break;
        default:
            break;
    }

    
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(pIrp);
    status = IoSetCompletionRoutineEx(pDeviceObject, pIrp, HumPnpCompletion, &Event, TRUE, TRUE, TRUE);
    if (NT_SUCCESS(status) == FALSE)
    {
        goto ExitUnlock;
    }

    NTSTATUS ioStatus = IoCallDriver(GET_NEXT_DEVICE_OBJECT(pDeviceObject), pIrp);
    if (ioStatus == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    status = pIrp->IoStatus.Status;//注意不是ioStatus
    MinorFunction = pStack->MinorFunction;
    switch (MinorFunction)
    {
        case IRP_MN_STOP_DEVICE:
        {
            RegDebug(L"IRP_MN_STOP_DEVICE", NULL, ++runtimes_step);
            pInterfaceInfo = pMiniDevExt->pInterfaceInfo;
            pMiniDevExt->PnpState = DEVICE_STATE_STOPPED;
            if (pMiniDevExt->pInterfaceInfo) {
                ExFreePool(pMiniDevExt->pInterfaceInfo);
                pMiniDevExt->pInterfaceInfo = NULL;
            }
            if (pMiniDevExt->pUsbDeviceDescriptor) {
                ExFreePool(pMiniDevExt->pUsbDeviceDescriptor);
                pMiniDevExt->pUsbDeviceDescriptor = NULL;
            }
            break;
        }
        case IRP_MN_QUERY_CAPABILITIES:
        {
            if (NT_SUCCESS(status) == TRUE)
            {
                pStack->Parameters.DeviceCapabilities.Capabilities->SurpriseRemovalOK = TRUE;
            }
            break;
        }
        case IRP_MN_START_DEVICE:
        {
            if (NT_SUCCESS(status) == FALSE)
            {
                pMiniDevExt->PnpState = DEVICE_STATE_START_FAILED;

            }
            else
            {
                pMiniDevExt->PnpState = DEVICE_STATE_RUNNING;
                status = HumInitDevice(pDeviceObject);
                if (NT_SUCCESS(status) == FALSE)
                {
                    DBGWARN(("HumInitDevice failed; failing IRP_MN_START_DEVICE."));
                    pMiniDevExt->PnpState = DEVICE_STATE_START_FAILED;
                }
            }
            break;
        }
    }
      
ExitUnlock:
    pIrp->IoStatus.Status = status;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    IoReleaseRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, sizeof(pMiniDevExt->RemoveLock));
    return status;
}


NTSTATUS HumGetReportDescriptor(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, BOOLEAN* NeedsCompletion)
{
    PDEVICE_EXTENSION       DeviceExtension;
    PIO_STACK_LOCATION      IrpStack;
    NTSTATUS                ntStatus = STATUS_SUCCESS;
    PVOID                   Report = NULL;
    ULONG                   ReportLength;
    ULONG                   bytesToCopy;
    USHORT Function;
    USHORT LangID;

    

    IrpStack = IoGetCurrentIrpStackLocation(pIrp);
    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);

    ReportLength = DeviceExtension->HidDescriptor.DescriptorList[0].wReportLength + 64;


    if (DeviceExtension->DeviceFlags & DEVICE_FLAGS_HID_1_0_D3_COMPAT_DEVICE) {
        PUSBD_PIPE_INFORMATION pipeInfo;

        pipeInfo = GetInterruptInputPipeForDevice(DeviceExtension);
        if (!pipeInfo) {
            ntStatus = STATUS_DEVICE_CONFIGURATION_ERROR;
            goto Exit;
        }

        UCHAR deviceInputEndpoint = pipeInfo->EndpointAddress & ~USB_ENDPOINT_DIRECTION_MASK;
        Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT;
        LangID = deviceInputEndpoint;
    }
    else {
        Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE;
        LangID = DeviceExtension->pInterfaceInfo->InterfaceNumber;   
    }

    ntStatus = HumGetDescriptorRequest(
        pDeviceObject,
        Function,
        DeviceExtension->HidDescriptor.DescriptorList[0].bReportType, // better be HID_REPORT_DESCRIPTOR_TYPE
        &Report,
        &ReportLength,
        sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
        0,      // Specify zero for all hid class descriptors except physical
        LangID); 

    if (NT_SUCCESS(ntStatus)) {
        bytesToCopy = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;

        if (bytesToCopy > DeviceExtension->HidDescriptor.DescriptorList[0].wReportLength) {
            bytesToCopy = DeviceExtension->HidDescriptor.DescriptorList[0].wReportLength;
        }

        if (bytesToCopy > ReportLength) {
            bytesToCopy = ReportLength;
        }

        RtlCopyMemory((PUCHAR)pIrp->UserBuffer, (PUCHAR)Report, bytesToCopy);
        pIrp->IoStatus.Information = bytesToCopy;


        //新增
        DeviceExtension->ReportDescLength = bytesToCopy;
        DeviceExtension->pReportDesc = (PVOID)ExAllocatePoolWithTag(NonPagedPoolNx, bytesToCopy, HIDUSB_TAG);
        if (DeviceExtension->pReportDesc) {
            RtlCopyMemory(DeviceExtension->pReportDesc, (PUCHAR)Report, DeviceExtension->ReportDescLength);
            RegDebug(L"HumGetReportDescriptor ReportDesc=", Report, bytesToCopy);
        }
        else {
            RegDebug(L"HumGetReportDescriptor  STATUS_INVALID_BUFFER_SIZE", NULL, ntStatus);
            ntStatus = STATUS_INVALID_BUFFER_SIZE;
        }

        ExFreePool(Report);
        return ntStatus;
    }
    if (ntStatus == STATUS_DEVICE_NOT_CONNECTED) {
        RegDebug(L"HumGetReportDescriptor  STATUS_DEVICE_NOT_CONNECTED", NULL, ntStatus);
        return ntStatus;
    }


Exit:
    pIrp->IoStatus.Status = ntStatus;
    if (HumQueueResetWorkItem(pDeviceObject, pIrp) == STATUS_MORE_PROCESSING_REQUIRED)
    {
        *NeedsCompletion = FALSE;
        IoReleaseRemoveLockEx(&DeviceExtension->RemoveLock, pIrp, sizeof(DeviceExtension->RemoveLock));
        return STATUS_PENDING;
    }
    RegDebug(L"HumGetReportDescriptor exit", NULL, ntStatus);
    return ntStatus;
}

NTSTATUS HumGetDeviceAttributes(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{
    NTSTATUS ntStatus;
    PDEVICE_EXTENSION   deviceExtension;
    PIO_STACK_LOCATION  irpStack;
    PHID_DEVICE_ATTRIBUTES deviceAttributes;

    

    irpStack = IoGetCurrentIrpStackLocation(pIrp);
    deviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
    deviceAttributes = (PHID_DEVICE_ATTRIBUTES)pIrp->UserBuffer;

    if (irpStack->Parameters.DeviceIoControl.OutputBufferLength >=
        sizeof(HID_DEVICE_ATTRIBUTES)) {

        //
        // Report how many bytes were copied
        //
        pIrp->IoStatus.Information = sizeof(HID_DEVICE_ATTRIBUTES);

        deviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
        deviceAttributes->VendorID = deviceExtension->pUsbDeviceDescriptor->idVendor;
        deviceAttributes->ProductID = deviceExtension->pUsbDeviceDescriptor->idProduct;
        deviceAttributes->VersionNumber = deviceExtension->pUsbDeviceDescriptor->bcdDevice;
        RegDebug(L"HumGetDeviceAttributes deviceAttributes=", deviceAttributes, sizeof(HID_DEVICE_ATTRIBUTES));
        ntStatus = STATUS_SUCCESS;
    }
    else {
        RegDebug(L"HumGetDeviceAttributes STATUS_INVALID_BUFFER_SIZE", NULL, runtimes_ioctl);
        ntStatus = STATUS_INVALID_BUFFER_SIZE;
    }

    return ntStatus;
}


NTSTATUS HumGetHidDescriptor(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{
    NTSTATUS ntStatus;
    PDEVICE_EXTENSION   DeviceExtension;
    PIO_STACK_LOCATION  IrpStack;

    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);

    if (DeviceExtension->HidDescriptor.bLength > 0) {
        IrpStack = IoGetCurrentIrpStackLocation(pIrp);
        ULONG bytesToCopy = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;
        if (bytesToCopy > DeviceExtension->HidDescriptor.bLength) {
            bytesToCopy = DeviceExtension->HidDescriptor.bLength;
        }

        RtlCopyMemory((PUCHAR)pIrp->UserBuffer, (PUCHAR)&DeviceExtension->HidDescriptor, bytesToCopy);
        pIrp->IoStatus.Information = bytesToCopy;
        ntStatus = STATUS_SUCCESS;
    }
    else {
        RegDebug(L"HumGetHidDescriptor STATUS_UNSUCCESSFUL", NULL, runtimes_ioctl);
        pIrp->IoStatus.Information = 0;
        ntStatus = STATUS_UNSUCCESSFUL;
    }

    return ntStatus;
}


NTSTATUS HumAbortPendingRequests(IN PDEVICE_OBJECT pDeviceObject)
{
    NTSTATUS                    status;
    USHORT                      UrbLen;
    PDEVICE_EXTENSION           pMiniDevExt;
    PURB                        pUrb;
    PUSBD_INTERFACE_INFORMATION pInterfaceInfo;
    USBD_PIPE_HANDLE            PipeHandle;
    PHID_DEVICE_EXTENSION       pDevExt;

    pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
    pMiniDevExt = (PDEVICE_EXTENSION)pDevExt->MiniDeviceExtension;
    UrbLen = sizeof(struct _URB_PIPE_REQUEST);
    pUrb = (PURB)ExAllocatePoolWithTag(NonPagedPoolNx, UrbLen, HIDUSB_TAG);
    if (pUrb == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pInterfaceInfo = pMiniDevExt->pInterfaceInfo;
    if (pInterfaceInfo == NULL || pInterfaceInfo->NumberOfPipes == 0)
    {
        status = STATUS_NO_SUCH_DEVICE;
    }
    else
    {
        PipeHandle = pInterfaceInfo->Pipes[0].PipeHandle;
        if (PipeHandle)
        {
            status = STATUS_NO_SUCH_DEVICE;
        }
        else
        {
            pUrb->UrbHeader.Length = UrbLen;
            pUrb->UrbHeader.Function = URB_FUNCTION_ABORT_PIPE;
            pUrb->UrbPipeRequest.PipeHandle = PipeHandle;
            status = HumCallUSB(pDeviceObject, pUrb);
            if (NT_SUCCESS(status) == FALSE)
            {
            }
        }
    }

    ExFreePool(pUrb);

    return status;
}

NTSTATUS HumRemoveDevice(IN PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
    NTSTATUS                ntStatus;
    ULONG                   PrevPnpState;
    PHID_DEVICE_EXTENSION   pDevExt;
    PDEVICE_EXTENSION       pMiniDevExt;

    RegDebug(L"HumRemoveDevice Entry", NULL, ++runtimes_step);
    pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
    pMiniDevExt = (PDEVICE_EXTENSION)pDevExt->MiniDeviceExtension;

    PrevPnpState = pMiniDevExt->PnpState;
    pMiniDevExt->PnpState = DEVICE_STATE_REMOVING;

    /*
 *  Note: RemoveDevice does an extra decrement, so we complete
 *        the REMOVE IRP on the transition to -1, whether this
 *        happens in RemoveDevice itself or subsequently while
 *        RemoveDevice is waiting for this event to fire.
 */
    if ((UINT32)(PrevPnpState-3) > 1) {//PrevPnpState != DEVICE_STATE_STOPPING && PrevPnpState != DEVICE_STATE_STOPPED??
        HumDecrementPendingRequestCount(pMiniDevExt);
    }

    //HumCompleteDeviceResetNotificationIrp(pDeviceObject, STATUS_NO_SUCH_DEVICE);

    if (PrevPnpState == DEVICE_STATE_RUNNING)
    {
        HumAbortPendingRequests(pDeviceObject);
    }
    KeWaitForSingleObject(&pMiniDevExt->AllRequestsCompleteEvent, Executive, KernelMode, FALSE, NULL);

    IoSkipCurrentIrpStackLocation(pIrp);//    IoCopyCurrentIrpStackLocationToNext(pIrp);??
    pIrp->IoStatus.Status = 0;
    ntStatus = IofCallDriver(pDevExt->NextDeviceObject, pIrp);
    IoReleaseRemoveLockAndWaitEx(&pMiniDevExt->RemoveLock, pIrp, sizeof(pMiniDevExt->RemoveLock));

    if (pMiniDevExt->pInterfaceInfo)
    {
        ExFreePool(pMiniDevExt->pInterfaceInfo);
        pMiniDevExt->pInterfaceInfo = NULL;
    }
    if (pMiniDevExt->pUsbDeviceDescriptor)
    {
        ExFreePool(pMiniDevExt->pUsbDeviceDescriptor);
        pMiniDevExt->pUsbDeviceDescriptor = NULL;
    }

    RegDebug(L"HumRemoveDevice end", NULL, runtimes_step);
    return ntStatus;
}


NTSTATUS HumStopDevice(IN PDEVICE_OBJECT pDeviceObject)
{
    PURB        pUrb;
    ULONG       Size;
    NTSTATUS    ntStatus = STATUS_SUCCESS;
    PDEVICE_EXTENSION DeviceExtension;

    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
    DeviceExtension->PnpState = DEVICE_STATE_STOPPING;

    RegDebug(L"HumStopDevice Entry", NULL, ++runtimes_step);
    /*
     *  Abort all pending IO on the device.
     *  We do an extra decrement here, which causes the
     *  NumPendingRequests to eventually go to -1, which causes
     *  AllRequestsCompleteEvent to get set.
     *  NumPendingRequests will get reset to 0 when we re-start.
     */
    HumAbortPendingRequests(pDeviceObject);
    HumDecrementPendingRequestCount(DeviceExtension);
    KeWaitForSingleObject(&DeviceExtension->AllRequestsCompleteEvent,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    /*
     *  Submit an open configuration Urb to the USB stack
     *  (with a NULL pointer for the configuration handle).
     */
    Size = sizeof(struct _URB_SELECT_CONFIGURATION);
    pUrb = ExAllocatePoolWithTag(NonPagedPoolNx, Size, HIDUSB_TAG);
    if (pUrb) {
        UsbBuildSelectConfigurationRequest(pUrb, (USHORT)Size, NULL);

        ntStatus = HumCallUSB(pDeviceObject, pUrb);

        ExFreePool(pUrb);
        if (NT_SUCCESS(ntStatus)) {
            return ntStatus;
        }
    }
    else {
        ntStatus = STATUS_UNSUCCESSFUL;
    }

    RegDebug(L"HumStopDevice end", NULL, runtimes_step);
    DeviceExtension->PnpState = DEVICE_STATE_STOPPED;
    return ntStatus;
}


NTSTATUS HumSystemControl(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{
    NTSTATUS            status = STATUS_SUCCESS;
    PHID_DEVICE_EXTENSION pDevExt;

    pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;

    //PIO_STACK_LOCATION  thisStackLoc;

    //thisStackLoc = IoGetCurrentIrpStackLocation(Irp);

    //switch (thisStackLoc->Parameters.DeviceIoControl.IoControlCode) {

    //    default:
    //        /*
    //         *  Note: do not return STATUS_NOT_SUPPORTED;
    //         *  If completing the IRP here,
    //         *  just keep the default status
    //         *  (this allows filter drivers to work).
    //         */
    //        status = pIrp->IoStatus.Status;
    //        break;
    //}


    IoCopyCurrentIrpStackLocationToNext(pIrp);
    status = IoCallDriver(pDevExt->NextDeviceObject, pIrp);

    return status;
}


NTSTATUS HumGetMsGenreDescriptor(
    IN PDEVICE_OBJECT pDeviceObject,
    IN PIRP pIrp)
{
    NTSTATUS ntStatus;
    PIO_STACK_LOCATION IrpStack;
    ULONG bufferSize;
    PDEVICE_EXTENSION DeviceExtension;

    

    DeviceExtension = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
    IrpStack = IoGetCurrentIrpStackLocation(pIrp);

    DBGOUT(("Received request for genre descriptor in hidusb"))

        /*
         *  Check buffer size before trying to use Irp->MdlAddress.
         */
    bufferSize = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;
    if (!bufferSize) {
        return STATUS_INVALID_USER_BUFFER;
    }

    PVOID pMappedBuff = HumGetSystemAddressForMdlSafe(pIrp->MdlAddress);
    if (!pMappedBuff) {
        return STATUS_INVALID_USER_BUFFER;
    }

    PURB pUrb = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST), HIDUSB_TAG);
    if (!pUrb) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(pUrb, sizeof(struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST));
    RtlZeroMemory(pMappedBuff, bufferSize);

    
    //HumBuildOsFeatureDescriptorRequest(pUrb,
    //                                    sizeof(struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST),
    //                                    DeviceExtension->pInterfaceInfo->InterfaceNumber,
    //                                    MS_GENRE_DESCRIPTOR_INDEX,
    //                                   pMappedBuff,
    //                                    NULL,
    //                                    bufferSize,
    //                                    NULL);

    pUrb->UrbHeader.Length = sizeof(struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST);//UrbLen
    pUrb->UrbHeader.Function = URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR;
    pUrb->UrbOSFeatureDescriptorRequest.TransferBufferMDL = NULL;
    pUrb->UrbOSFeatureDescriptorRequest.Recipient = 1;//
    pUrb->UrbOSFeatureDescriptorRequest.TransferBufferLength = bufferSize;
    pUrb->UrbOSFeatureDescriptorRequest.TransferBuffer = pMappedBuff;
    pUrb->UrbOSFeatureDescriptorRequest.InterfaceNumber = DeviceExtension->pInterfaceInfo->InterfaceNumber;
    pUrb->UrbOSFeatureDescriptorRequest.MS_FeatureDescriptorIndex = MS_GENRE_DESCRIPTOR_INDEX;//MS_GENRE_DESCRIPTOR_INDEX=1
    pUrb->UrbOSFeatureDescriptorRequest.UrbLink = 0;

    DBGOUT(("Sending os feature request to usbhub"))
    ntStatus = HumCallUSB(pDeviceObject, pUrb);
    if (NT_SUCCESS(ntStatus)) {
        if (USBD_SUCCESS(pUrb->UrbHeader.Status)) {
            DBGOUT(("Genre descriptor request successful!"))

            pIrp->IoStatus.Information = pUrb->UrbOSFeatureDescriptorRequest.TransferBufferLength;
            ntStatus = STATUS_SUCCESS;
        }
        else {
            DBGOUT(("Genre descriptor request unsuccessful"))
            ntStatus = STATUS_UNSUCCESSFUL;
        }
    }

    ExFreePool(pUrb);

    return ntStatus;
}

NTSTATUS HumGetPhysicalDescriptor(IN PDEVICE_OBJECT pDeviceObject,
                                  IN PIRP pIrp,
                                  BOOLEAN* NeedsCompletion)
{
    UNREFERENCED_PARAMETER(NeedsCompletion);

    NTSTATUS ntStatus;
    PIO_STACK_LOCATION IrpStack;
    ULONG bufferSize;

    

    IrpStack = IoGetCurrentIrpStackLocation(pIrp);


    /*
     *  Check buffer size before trying to use Irp->MdlAddress.
     */
    bufferSize = IrpStack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = HumGetSystemAddressForMdlSafe(pIrp->MdlAddress);
    if (bufferSize!=0 && buffer!=NULL) {
        ntStatus = HumGetDescriptorRequest(pDeviceObject,
                                            URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE,
                                            HID_PHYSICAL_DESCRIPTOR_TYPE,
                                            &buffer,
                                            &bufferSize,
                                            sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                            0, // Index. NOTE: will only get first physical descriptor set
                                            0);

    }
    else {
        ntStatus = STATUS_INVALID_USER_BUFFER;
    }

    return ntStatus;
}

//
//VOID HumCompleteDeviceResetNotificationIrp(IN PDEVICE_OBJECT pDeviceObject, IN NTSTATUS ntStatus)
//{
//    PDEVICE_EXTENSION pMiniDevExt;
//    PIRP pIrpReset = NULL;
//
//    pMiniDevExt = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
//    KIRQL newIRQL = KeAcquireSpinLockRaiseToDpc(&pMiniDevExt->DeviceResetNotificationSpinLock);
//    if (pMiniDevExt->pDeviceResetNotificationIrp)
//    {
//        pIrpReset = pMiniDevExt->pDeviceResetNotificationIrp;
//        pMiniDevExt->pDeviceResetNotificationIrp = NULL;
//    }
//    KeReleaseSpinLock(&pMiniDevExt->DeviceResetNotificationSpinLock, newIRQL);
//    if (pIrpReset)
//    {
//        InterlockedExchange64((PVOID)&pIrpReset->CancelRoutine, 0);
//
//        pIrpReset->IoStatus.Status = ntStatus;
//        IofCompleteRequest(pIrpReset, 0);
//        IoReleaseRemoveLockEx(&pMiniDevExt->RemoveLock, pIrpReset, sizeof(pMiniDevExt->RemoveLock));
//    }
//}


//VOID HumDeviceResetNotificationIrpCancelled(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp) //void** __fastcall ??
//{
//    PDEVICE_EXTENSION pMiniDevExt;
//
//    BOOLEAN bNeedsCompletion;
//
//    //void** result;//PVOID* result
//
//    IoReleaseCancelSpinLock(pIrp->CancelIrql);
//    bNeedsCompletion = FALSE;
//    pMiniDevExt = GET_MINIDRIVER_DEVICE_EXTENSION(pDeviceObject);
//    KIRQL newIRQL = KeAcquireSpinLockRaiseToDpc(&pMiniDevExt->DeviceResetNotificationSpinLock);
//    if (pMiniDevExt->pDeviceResetNotificationIrp == pIrp)
//    {
//        pMiniDevExt->pDeviceResetNotificationIrp = NULL;
//        bNeedsCompletion = TRUE;
//    }
//    KeReleaseSpinLock(&pMiniDevExt->DeviceResetNotificationSpinLock, newIRQL);
//    if (bNeedsCompletion)
//    {
//        pIrp->IoStatus.Information = 0;
//        pIrp->IoStatus.Status = STATUS_CANCELLED;
//        IofCompleteRequest(pIrp, 0);
//        IoReleaseRemoveLockEx(&pMiniDevExt->RemoveLock, pIrp, sizeof(pMiniDevExt->RemoveLock));
//    }
//    
//    //return result;
//}



VOID HumResetWorkItem(IN PDEVICE_OBJECT pDeviceObject, IN PVOID Context)
{
    UNREFERENCED_PARAMETER(pDeviceObject);

    resetWorkItemContext* resetWorkItemObj;
    PHID_DEVICE_EXTENSION pDevExt;
    PDEVICE_EXTENSION pMiniDevExt;
    NTSTATUS ntStatus;
    ULONG portStatus = 0;
    BOOLEAN bNeedsReset;

    /*
     *  Get the information out of the resetWorkItemContext and free it.
     */
    resetWorkItemObj = (resetWorkItemContext*)Context;

    //pMiniDevExt = GET_MINIDRIVER_DEVICE_EXTENSION(resetWorkItemObj->deviceObject);

    pDevExt = resetWorkItemObj->deviceObject->DeviceExtension;//
    pMiniDevExt = (PDEVICE_EXTENSION)pDevExt->MiniDeviceExtension;

    bNeedsReset = FALSE;//??

    ntStatus = HumIncrementPendingRequestCount(pMiniDevExt);
    if (NT_SUCCESS(ntStatus)) {
        // Check the port state, if it is disabled we will need 
        // to re-enable it
        ntStatus = HumGetPortStatus(resetWorkItemObj->deviceObject, &portStatus);
        if (!NT_SUCCESS(ntStatus)) {
            DBGWARN(("HumResetWorkItem: HumGetPortStatus failed with status %xh.", ntStatus));
            HumDecrementPendingRequestCount(pMiniDevExt);
            goto Exit;
        }
        if ((portStatus & USBD_PORT_CONNECTED) == 0) {
            HumDecrementPendingRequestCount(pMiniDevExt);
            goto Exit;
        }

        DBGPRINT(1, ("Attempting port reset"));
        ntStatus = HumAbortPendingRequests(resetWorkItemObj->deviceObject);
        if (!NT_SUCCESS(ntStatus)) {
            DBGWARN(("HumResetWorkItem: HumAbortPendingRequests failed with status %xh.", ntStatus));
            HumDecrementPendingRequestCount(pMiniDevExt);
            goto Exit;
        }

        ntStatus = HumResetParentPort(resetWorkItemObj->deviceObject);
        if (ntStatus == STATUS_DEVICE_DATA_ERROR)
        {
            pMiniDevExt->PnpState = DEVICE_STATE_START_FAILED;
            IoInvalidateDeviceState(pDevExt->PhysicalDeviceObject);

            HumDecrementPendingRequestCount(pMiniDevExt);
            goto Exit;
        }

        // now attempt to reset the stalled pipe, this will clear the stall
        // on the device as well.

        /*
            *  This call does not close the endpoint, so it should be ok
            *  to make this call whether or not we succeeded in aborting
            *  all pending IO.
            */
        if (NT_SUCCESS(ntStatus)) {
            bNeedsReset = TRUE;
            ntStatus = HumResetInterruptPipe(resetWorkItemObj->deviceObject);
            goto Exit;
        }

        HumDecrementPendingRequestCount(pMiniDevExt);
    }


Exit:
    /*
     *  Clear the ResetWorkItem ptr in the device extension
     *  AFTER resetting the pipe so we don't end up with
     *  two threads resetting the same pipe at the same time.
     */
    InterlockedExchange((PVOID)&pMiniDevExt->pResetWorkItem, 0);


    //if (bNeedsReset) {
    //    HumCompleteDeviceResetNotificationIrp(pDeviceObject, STATUS_SUCCESS);
    //}


    /*
     *  The IRP that returned the error which prompted us to do this reset
     *  is still owned by HIDUSB because we returned
     *  STATUS_MORE_PROCESSING_REQUIRED in the completion routine.
     *  Now that the hub is reset, complete this failed IRP.
     */
    DBGPRINT(1, ("Completing IRP %ph following port reset", resetWorkItemObj->irpToComplete));
    IoCompleteRequest(resetWorkItemObj->irpToComplete, IO_NO_INCREMENT);

    IoFreeWorkItem(resetWorkItemObj->ioWorkItem);
    ExFreePool(resetWorkItemObj);

    IoReleaseRemoveLockEx(&pMiniDevExt->RemoveLock, (PVOID)HID_REMLOCK_TAG, sizeof(pMiniDevExt->RemoveLock));
}


NTSTATUS HumQueueResetWorkItem(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp)
{
    PHID_DEVICE_EXTENSION pDevExt;
    PDEVICE_EXTENSION pMiniDevExt;

    resetWorkItemContext* resetWorkItemObj;
    PIO_WORKITEM pWorkItem; // rax

    pDevExt = (PHID_DEVICE_EXTENSION)pDeviceObject->DeviceExtension;
    pMiniDevExt = (PDEVICE_EXTENSION)pDevExt->MiniDeviceExtension;
    RegDebug(L"HumQueueResetWorkItem start", NULL, runtimes_ioctl);
    if (NT_SUCCESS(HumIncrementPendingRequestCount(pMiniDevExt)) == FALSE)//NT_SUCCESS(HumIncrementPendingRequestCount(pMiniDevExt)) == FALSE//(int)HumIncrementPendingRequestCount(pMiniDevExt) < 0
    {
        RegDebug(L"HumQueueResetWorkItem HumIncrementPendingRequestCount <0", NULL, runtimes_ioctl);
        return STATUS_SUCCESS;
    }
    resetWorkItemObj = (resetWorkItemContext*)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(struct tag_resetWorkItemContext), HIDUSB_TAG);//0x28 //40
    if (!resetWorkItemObj)
    {
        RegDebug(L"HumQueueResetWorkItem resetWorkItemObj err", NULL, runtimes_ioctl);
        HumDecrementPendingRequestCount(pMiniDevExt);
        return STATUS_SUCCESS;
    }
    pWorkItem = IoAllocateWorkItem(pMiniDevExt->pFDO);
    resetWorkItemObj->ioWorkItem = pWorkItem;
    if (!pWorkItem)
    {
        RegDebug(L"HumQueueResetWorkItem pWorkItem err", NULL, runtimes_ioctl);
        ExFreePool(resetWorkItemObj);
        HumDecrementPendingRequestCount(pMiniDevExt);
        return STATUS_SUCCESS;
    }
    if (InterlockedCompareExchange64((PVOID)&pMiniDevExt->pResetWorkItem, (LONG64)resetWorkItemObj->ioWorkItem, 0))
    {
        RegDebug(L"HumQueueResetWorkItem InterlockedCompareExchange64", NULL, runtimes_ioctl);
        IoFreeWorkItem(resetWorkItemObj->ioWorkItem);
        ExFreePool(resetWorkItemObj);
        HumDecrementPendingRequestCount(pMiniDevExt);
        return STATUS_SUCCESS;
    }

    resetWorkItemObj->sig = RESET_WORK_ITEM_CONTEXT_SIG; // 'tesR'
    resetWorkItemObj->irpToComplete = pIrp;
    resetWorkItemObj->deviceObject = pDeviceObject;
    IoMarkIrpPending(pIrp);

    IoAcquireRemoveLockEx(&pMiniDevExt->RemoveLock, (PVOID)HID_REMLOCK_TAG, __FILE__, __LINE__, sizeof(pMiniDevExt->RemoveLock));
    IoQueueWorkItem(resetWorkItemObj->ioWorkItem, (PIO_WORKITEM_ROUTINE)HumResetWorkItem, DelayedWorkQueue, resetWorkItemObj);
    RegDebug(L"HumQueueResetWorkItem end", NULL, runtimes_ioctl);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

