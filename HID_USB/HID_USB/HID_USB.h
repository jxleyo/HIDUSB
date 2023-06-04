#ifndef __HIDUSB_H__
#define __HIDUSB_H__


#include <ntifs.h>
#include <hidport.h>
#include <Usb.h>
#include <usbioctl.h>
#include <Usbdlib.h>


#include <PSHPACK1.H>

typedef struct _USB_HID_DESCRIPTOR
{
    UCHAR   bLength;
    UCHAR   bDescriptorType;
    USHORT  bcdHID;
    UCHAR   bCountry;
    UCHAR   bNumDescriptors;
    UCHAR   bReportType;
    USHORT  wReportLength;

} USB_HID_DESCRIPTOR, * PUSB_HID_DESCRIPTOR;

#include <POPPACK.H>


//
// Device Class Constants for HID
//
#define HID_GET_REPORT      0x01
#define HID_GET_IDLE        0x02
#define HID_GET_PROTOCOL    0x03

#define HID_SET_REPORT      0x09
#define HID_SET_IDLE        0x0A
#define HID_SET_PROTOCOL    0x0B



#define BAD_POINTER ((PVOID)0xFFFFFFFE)

/*
 *  HIDUSB signature tag for memory allocations
 */
#define HIDUSB_TAG (ULONG)'UdiH'//0x55646948
#define HID_REMLOCK_TAG (ULONG)'WRUH' //0x57525548
#define RESET_WORK_ITEM_CONTEXT_SIG 'tesR'



typedef struct _DEVICE_EXTENSION
{
    ULONG                           PnpState;// DeviceState;
    PUSB_DEVICE_DESCRIPTOR          pUsbDeviceDescriptor;
    PUSBD_INTERFACE_INFORMATION     pInterfaceInfo;
    USBD_CONFIGURATION_HANDLE       UsbdConfigurationHandle;

    LONG                            nPendingRequestsCount;
    KEVENT                          AllRequestsCompleteEvent;

    ULONG                           DeviceFlags;

    PIO_WORKITEM                    pResetWorkItem;//
    HID_DESCRIPTOR                  HidDescriptor;
    PUSB_INTERFACE_DESCRIPTOR       pInterfaceDesc;//??
    USB_CONFIGURATION_DESCRIPTOR    UsbConfigDesc;//新增
    
    PVOID                           pReportDesc;//新增
    ULONG                           ReportDescLength;//新增

    UCHAR                           PowerFlag;//DeviceFlags??PnpState
    UCHAR                           HidDescriptorLength;//??

    //3 bytes align
    PDEVICE_OBJECT                  pFDO;//functionalDeviceObject
    IO_REMOVE_LOCK                  RemoveLock;

    KSPIN_LOCK                      DeviceResetNotificationSpinLock;//resetWorkItemsListSpinLock??
    PIRP                            pDeviceResetNotificationIrp;

} DEVICE_EXTENSION, * PDEVICE_EXTENSION;


/*
 *  This structure is used to pass information to the 
 *  resetWorkItem callback.
 */
typedef struct tag_resetWorkItemContext {
                    ULONG sig;//Tag
                    PIO_WORKITEM ioWorkItem;
                    PDEVICE_OBJECT deviceObject;
                    PIRP irpToComplete;

                    //struct tag_resetWorkItemContext *next;
} resetWorkItemContext;

#define DEVICE_STATE_NONE           0
#define DEVICE_STATE_STARTING       1
#define DEVICE_STATE_RUNNING        2
#define DEVICE_STATE_STOPPING       3
#define DEVICE_STATE_STOPPED        4
#define DEVICE_STATE_REMOVING       5
#define DEVICE_STATE_START_FAILED   6

#define DEVICE_FLAGS_HID_1_0_D3_COMPAT_DEVICE   0x00000001

//
// Interface slection options
//
#define HUM_SELECT_DEFAULT_INTERFACE    0
#define HUM_SELECT_SPECIFIED_INTERFACE  1

//
// Device Extension Macros
//

#define GET_MINIDRIVER_DEVICE_EXTENSION(DO) ((PDEVICE_EXTENSION) (((PHID_DEVICE_EXTENSION)(DO)->DeviceExtension)->MiniDeviceExtension))

#define GET_HIDCLASS_DEVICE_EXTENSION(DO) ((PHID_DEVICE_EXTENSION)(DO)->DeviceExtension)

#define GET_NEXT_DEVICE_OBJECT(DO) (((PHID_DEVICE_EXTENSION)(DO)->DeviceExtension)->NextDeviceObject)


#define DBGPRINT(lvl, arg)
#define DBGWARN(args_in_parens)                                
#define DBGERR(args_in_parens)                                
#define DBGOUT(args_in_parens)                                


#define HumBuildGetDescriptorRequest(urb, \
                                     function, \
                                     length, \
                                     descriptorType, \
                                     index, \
                                     languageId, \
                                     transferBuffer, \
                                     transferBufferMDL, \
                                     transferBufferLength, \
                                     link) { \
            (urb)->UrbHeader.Function =  (function); \
            (urb)->UrbHeader.Length = (length); \
            (urb)->UrbControlDescriptorRequest.TransferBufferLength = (transferBufferLength); \
            (urb)->UrbControlDescriptorRequest.TransferBufferMDL = (transferBufferMDL); \
            (urb)->UrbControlDescriptorRequest.TransferBuffer = (transferBuffer); \
            (urb)->UrbControlDescriptorRequest.DescriptorType = (descriptorType); \
            (urb)->UrbControlDescriptorRequest.Index = (index); \
            (urb)->UrbControlDescriptorRequest.LanguageId = (languageId); \
            (urb)->UrbControlDescriptorRequest.UrbLink = (link); }


#define HumBuildClassRequest(urb, \
                                       function, \
                                       transferFlags, \
                                       transferBuffer, \
                                       transferBufferLength, \
                                       requestType, \
                                       request, \
                                       value, \
                                       index, \
                                       reqLength){ \
            (urb)->UrbHeader.Length = (USHORT) sizeof( struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST); \
            (urb)->UrbHeader.Function = function; \
            (urb)->UrbControlVendorClassRequest.Index = (index); \
            (urb)->UrbControlVendorClassRequest.RequestTypeReservedBits = (requestType); \
            (urb)->UrbControlVendorClassRequest.Request = (request); \
            (urb)->UrbControlVendorClassRequest.Value = (value); \
            (urb)->UrbControlVendorClassRequest.TransferFlags = (transferFlags); \
            (urb)->UrbControlVendorClassRequest.TransferBuffer = (transferBuffer); \
            (urb)->UrbControlVendorClassRequest.TransferBufferLength = (transferBufferLength); }

#define HumBuildSelectConfigurationRequest(urb, \
                                         length, \
                                         configurationDescriptor) { \
            (urb)->UrbHeader.Function =  URB_FUNCTION_SELECT_CONFIGURATION; \
            (urb)->UrbHeader.Length = (length); \
            (urb)->UrbSelectConfiguration.ConfigurationDescriptor = (configurationDescriptor);    }

#define HumBuildOsFeatureDescriptorRequest(urb, \
                              length, \
                              interface, \
                              index, \
                              transferBuffer, \
                              transferBufferMDL, \
                              transferBufferLength, \
                              link) { \
            (urb)->UrbHeader.Function = URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR; \
            (urb)->UrbHeader.Length = (length); \
            (urb)->UrbOSFeatureDescriptorRequest.TransferBufferLength = (transferBufferLength); \
            (urb)->UrbOSFeatureDescriptorRequest.TransferBufferMDL = (transferBufferMDL); \
            (urb)->UrbOSFeatureDescriptorRequest.TransferBuffer = (transferBuffer); \
            (urb)->UrbOSFeatureDescriptorRequest.InterfaceNumber = (interface); \
            (urb)->UrbOSFeatureDescriptorRequest.MS_FeatureDescriptorIndex = (index); \
            (urb)->UrbOSFeatureDescriptorRequest.UrbLink = (link); }




//
// Function prototypes
//

NTSTATUS    DriverEntry(IN PDRIVER_OBJECT pDriverObject, IN PUNICODE_STRING registryPath);
NTSTATUS    HumAbortPendingRequests(IN PDEVICE_OBJECT pDeviceObject);
NTSTATUS    HumCreateClose(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumInternalIoctl(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumPnP(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumPower(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumAddDevice(IN PDRIVER_OBJECT pDriverObject, IN PDEVICE_OBJECT pFunctionalDeviceObject);
NTSTATUS    HumPnpCompletion(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, IN PVOID Context);
NTSTATUS    HumInitDevice(IN PDEVICE_OBJECT pDeviceObject);
NTSTATUS    HumStopDevice(IN PDEVICE_OBJECT pDeviceObject);
NTSTATUS    HumRemoveDevice(IN PDEVICE_OBJECT pDeviceObject, PIRP pIrp);
NTSTATUS    HumCallUSB(IN PDEVICE_OBJECT pDeviceObject, IN PURB pUrb);
VOID        HumUnload(IN PDRIVER_OBJECT DriverObject);
NTSTATUS    HumGetHidDescriptor(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumGetReportDescriptor(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, BOOLEAN* NeedsCompletion);
NTSTATUS    HumReadReport(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, BOOLEAN* NeedsCompletion);
NTSTATUS    HumReadCompletion(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, IN PVOID Context);
NTSTATUS    HumWriteReport(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, BOOLEAN* NeedsCompletion);
NTSTATUS    HumGetSetReport(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, BOOLEAN* NeedsCompletion);
NTSTATUS    HumWriteCompletion(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, IN PVOID Context);
NTSTATUS    HumGetDeviceAttributes(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumGetDescriptorRequest(IN PDEVICE_OBJECT pDeviceObject, IN USHORT Function, IN CHAR DescriptorType, IN OUT PVOID* pDescBuffer, IN OUT ULONG* pDescBuffLen, IN INT TypeSize, IN CHAR Index, IN USHORT LangID);
NTSTATUS    HumSetIdle(IN PDEVICE_OBJECT pDeviceObject);
VOID        HumSetIdleWorker(PDEVICE_OBJECT pDeviceObject, PVOID Context);
NTSTATUS    HumSelectConfiguration(IN PDEVICE_OBJECT DeviceObject, IN PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor);
NTSTATUS    HumParseHidInterface(IN PDEVICE_EXTENSION DeviceExtension, IN PUSB_INTERFACE_DESCRIPTOR InterfaceDesc, IN ULONG InterfaceLength, OUT PUSB_CONFIGURATION_DESCRIPTOR* pUsbConfigDesc);
NTSTATUS    HumGetDeviceDescriptor(IN PDEVICE_OBJECT pDeviceObject, IN PDEVICE_EXTENSION DeviceExtension);
NTSTATUS    HumGetConfigDescriptor(IN PDEVICE_OBJECT pDeviceObject, OUT PUSB_CONFIGURATION_DESCRIPTOR *ppConfigurationDesc, OUT PULONG pConfigurationDescLength);

LONG        HumDecrementPendingRequestCount(IN PDEVICE_EXTENSION DeviceExtension);
NTSTATUS    HumIncrementPendingRequestCount(IN PDEVICE_EXTENSION DeviceExtension);
VOID        HumResetWorkItem(IN PDEVICE_OBJECT pDeviceObject, IN PVOID Context);
NTSTATUS    HumResetParentPort(IN PDEVICE_OBJECT pDeviceObject);
NTSTATUS    HumGetPortStatus(IN PDEVICE_OBJECT pDeviceObject, IN PULONG pPortStatus);
NTSTATUS    HumResetInterruptPipe(IN PDEVICE_OBJECT pDeviceObject);
NTSTATUS    HumSystemControl(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumGetStringDescriptor(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumGetPhysicalDescriptor(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, BOOLEAN *NeedsCompletion);
NTSTATUS    HumGetMsGenreDescriptor(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumSendIdleNotificationRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp, BOOLEAN* NeedsCompletion, BOOLEAN* AcquiredLock);

//extern KSPIN_LOCK resetWorkItemsListSpinLock;

NTSTATUS    HumGetSetReportCompletion(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp, IN PVOID Context);
NTSTATUS    HumPowerCompletion(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);
NTSTATUS    HumQueueResetWorkItem(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);

//VOID HumCompleteDeviceResetNotificationIrp(IN PDEVICE_OBJECT pDeviceObject, IN NTSTATUS ntStatus);
//VOID HumDeviceResetNotificationIrpCancelled(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp);//void** __fastcall ??


#endif 
