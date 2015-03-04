/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#define INITGUID 1

#include <ntddk.h>
#include <wdmguid.h>
#include <ntstrsafe.h>
#include <util.h>

#include <emulated_interface.h>
#include <unplug_interface.h>

#include "names.h"
#include "fdo.h"
#include "pdo.h"
#include "bus.h"
#include "driver.h"
#include "thread.h"
#include "registry.h"
#include "dbg_print.h"
#include "assert.h"

#define PDO_TAG 'ODP'

#define MAXNAMELEN  128

struct _XENBUS_PDO {
    PXENBUS_DX                  Dx;

    PXENBUS_THREAD              SystemPowerThread;
    PIRP                        SystemPowerIrp;
    PXENBUS_THREAD              DevicePowerThread;
    PIRP                        DevicePowerIrp;

    PXENBUS_FDO                 Fdo;
    BOOLEAN                     Missing;
    const CHAR                  *Reason;

    BOOLEAN                     Removable;
    BOOLEAN                     Ejectable;

    PULONG                      Revision;
    PWCHAR                      *Description;
    ULONG                       Count;

    BUS_INTERFACE_STANDARD      BusInterface;

    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackLate;
};

static FORCEINLINE PVOID
__PdoAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, PDO_TAG);
}

static FORCEINLINE VOID
__PdoFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, PDO_TAG);
}

static FORCEINLINE VOID
__PdoSetDevicePnpState(
    IN  PXENBUS_PDO         Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENBUS_DX              Dx = Pdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

VOID
PdoSetDevicePnpState(
    IN  PXENBUS_PDO         Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    __PdoSetDevicePnpState(Pdo, State);
}

static FORCEINLINE VOID
__PdoRestoreDevicePnpState(
    IN  PXENBUS_PDO         Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENBUS_DX              Dx = Pdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__PdoGetDevicePnpState(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return Dx->DevicePnpState;
}

DEVICE_PNP_STATE
PdoGetDevicePnpState(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetDevicePnpState(Pdo);
}

static FORCEINLINE VOID
__PdoSetDevicePowerState(
    IN  PXENBUS_PDO         Pdo,
    IN  DEVICE_POWER_STATE  State
    )
{
    PXENBUS_DX              Dx = Pdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__PdoGetDevicePowerState(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__PdoSetSystemPowerState(
    IN  PXENBUS_PDO         Pdo,
    IN  SYSTEM_POWER_STATE  State
    )
{
    PXENBUS_DX              Dx = Pdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__PdoGetSystemPowerState(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return Dx->SystemPowerState;
}

static FORCEINLINE VOID
__PdoSetMissing(
    IN  PXENBUS_PDO Pdo,
    IN  const CHAR  *Reason
    )
{
    Pdo->Reason = Reason;
    Pdo->Missing = TRUE;
}

VOID
PdoSetMissing(
    IN  PXENBUS_PDO Pdo,
    IN  const CHAR  *Reason
    )
{
    __PdoSetMissing(Pdo, Reason);
}

static FORCEINLINE BOOLEAN
__PdoIsMissing(
    IN  PXENBUS_PDO Pdo
    )
{
    return Pdo->Missing;
}

BOOLEAN
PdoIsMissing(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoIsMissing(Pdo);
}

static FORCEINLINE VOID
__PdoSetName(
    IN  PXENBUS_PDO     Pdo,
    IN  PANSI_STRING    Name
    )
{
    PXENBUS_DX          Dx = Pdo->Dx;
    NTSTATUS            status;

    status = RtlStringCbPrintfA(Dx->Name,
                                MAX_DEVICE_ID_LEN,
                                "%Z",
                                Name);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PCHAR
__PdoGetName(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return Dx->Name;
}

PCHAR
PdoGetName(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetName(Pdo);
}

static FORCEINLINE VOID
__PdoSetRemovable(
    IN  PXENBUS_PDO     Pdo
    )
{
    HANDLE              ParametersKey;
    HANDLE              Key;
    ULONG               Value;
    NTSTATUS            status;

    Value = 1;

    ParametersKey = DriverGetParametersKey();

    status = RegistryOpenSubKey(ParametersKey,
                                __PdoGetName(Pdo),
                                KEY_READ,
                                &Key);
    if (!NT_SUCCESS(status))
        goto done;

    (VOID) RegistryQueryDwordValue(Key,
                                   "AllowPdoRemove",
                                   &Value);

    RegistryCloseKey(Key);

done:
    Pdo->Removable = (Value != 0) ? TRUE : FALSE;
}

static FORCEINLINE BOOLEAN
__PdoIsRemovable(
    IN  PXENBUS_PDO     Pdo
    )
{
    return Pdo->Removable;
}

static FORCEINLINE VOID
__PdoSetEjectable(
    IN  PXENBUS_PDO     Pdo
    )
{
    HANDLE              ParametersKey;
    HANDLE              Key;
    ULONG               Value;
    NTSTATUS            status;

    Value = 1;

    ParametersKey = DriverGetParametersKey();

    status = RegistryOpenSubKey(ParametersKey,
                                __PdoGetName(Pdo),
                                KEY_READ,
                                &Key);
    if (!NT_SUCCESS(status))
        goto done;

    (VOID) RegistryQueryDwordValue(Key,
                                   "AllowPdoEject",
                                   &Value);

    RegistryCloseKey(Key);

done:
    Pdo->Ejectable = (Value != 0) ? TRUE : FALSE;
}

static FORCEINLINE BOOLEAN
__PdoIsEjectable(
    IN  PXENBUS_PDO     Pdo
    )
{
    return Pdo->Ejectable;
}

#define MAXTEXTLEN  1024

static FORCEINLINE PXENBUS_FDO
__PdoGetFdo(
    IN  PXENBUS_PDO Pdo
    )
{
    return Pdo->Fdo;
}

PXENBUS_FDO
PdoGetFdo(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetFdo(Pdo);
}

static NTSTATUS
PdoAddRevision(
    IN  PXENBUS_PDO Pdo,
    IN  ULONG       Revision,
    IN  ULONG       Suspend,
    IN  ULONG       SharedInfo,
    IN  ULONG       Evtchn,
    IN  ULONG       Debug,
    IN  ULONG       Store,
    IN  ULONG       RangeSet,
    IN  ULONG       Cache,
    IN  ULONG       Gnttab,
    IN  ULONG       Emulated,
    IN  ULONG       Unplug
    )
{
    PVOID           Buffer;
    ULONG           Count;
    NTSTATUS        status;

    Count = Pdo->Count + 1;

    Buffer = __PdoAllocate(sizeof (ULONG) * Count);

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto fail1;

    if (Pdo->Revision != NULL) {
        RtlCopyMemory(Buffer,
                      Pdo->Revision,
                      sizeof (ULONG) * Pdo->Count);
        __PdoFree(Pdo->Revision);
    }

    Pdo->Revision = Buffer;
    Pdo->Revision[Pdo->Count] = Revision;

    Buffer = __PdoAllocate(sizeof (PCHAR) * Count);

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto fail2;

    if (Pdo->Description != NULL) {
        RtlCopyMemory(Buffer,
                      Pdo->Description,
                      sizeof (PWCHAR) * Pdo->Count);
        __PdoFree(Pdo->Description);
    }

    Pdo->Description = Buffer;

    Buffer = __PdoAllocate(MAXTEXTLEN * Count);

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto fail3;

    status = RtlStringCbPrintfW(Buffer,
                                MAXTEXTLEN,
                                L"%hs %hs: "
                                L"SUSPEND v%u "
                                L"SHARED_INFO v%u "
                                L"EVTCHN v%u "
                                L"DEBUG v%u "
                                L"STORE v%u "
                                L"RANGE_SET v%u "
                                L"CACHE v%u "
                                L"GNTTAB v%u "
                                L"EMULATED v%u "
                                L"UNPLUG v%u",
                                FdoGetName(__PdoGetFdo(Pdo)),
                                __PdoGetName(Pdo),
                                Suspend,
                                SharedInfo,
                                Evtchn,
                                Debug,
                                Store,
                                RangeSet,
                                Cache,
                                Gnttab,
                                Emulated,
                                Unplug);
    ASSERT(NT_SUCCESS(status));

    Pdo->Description[Pdo->Count] = Buffer;

    Trace("%08x -> %ws\n",
          Pdo->Revision[Pdo->Count],
          Pdo->Description[Pdo->Count]);

    Pdo->Count++;

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
PdoSetRevisions(
    IN  PXENBUS_PDO Pdo
    )
{
    ULONG           Suspend;
    ULONG           Revision;
    NTSTATUS        status;

    Revision = 0;

    // Enumerate all possible combinations of exported interface versions since v1
    // and add a PDO revsion for each combination that's currently supported. Note that
    // the exported interfaces include any interface queries we pass through.
    // We must enumerate from v1 to ensure that revision numbers don't change
    // even when a particular combination of interface versions becomes
    // unsupported. (See README.md for API versioning policy).

    for (Suspend = 1; Suspend <= XENBUS_SUSPEND_INTERFACE_VERSION_MAX; Suspend++) {
        ULONG   SharedInfo;
        
        for (SharedInfo = 1; SharedInfo <= XENBUS_SHARED_INFO_INTERFACE_VERSION_MAX; SharedInfo++) {
            ULONG   Evtchn;
            
            for (Evtchn = 1; Evtchn <= XENBUS_EVTCHN_INTERFACE_VERSION_MAX; Evtchn++) {
                ULONG   Debug;

                for (Debug = 1; Debug <= XENBUS_DEBUG_INTERFACE_VERSION_MAX; Debug++) {
                    ULONG   Store;

                    for (Store = 1; Store <= XENBUS_STORE_INTERFACE_VERSION_MAX; Store++) {
                        ULONG   RangeSet;

                        for (RangeSet = 1; RangeSet <= XENBUS_RANGE_SET_INTERFACE_VERSION_MAX; RangeSet++) {
                            ULONG   Cache;
                            
                            for (Cache = 1; Cache <= XENBUS_CACHE_INTERFACE_VERSION_MAX; Cache++) {
                                ULONG   Gnttab;
                            
                                for (Gnttab = 1; Gnttab <= XENBUS_GNTTAB_INTERFACE_VERSION_MAX; Gnttab++) {
                                    ULONG   Emulated;
                                
                                    for (Emulated = 1; Emulated <= XENFILT_EMULATED_INTERFACE_VERSION_MAX; Emulated++) {
                                        ULONG   Unplug;

                                        for (Unplug = 1; Unplug <= XENFILT_UNPLUG_INTERFACE_VERSION_MAX; Unplug++) {
                                            Revision++;

                                            if (Suspend >= XENBUS_SUSPEND_INTERFACE_VERSION_MIN &&
                                                SharedInfo >= XENBUS_SHARED_INFO_INTERFACE_VERSION_MIN &&
                                                Evtchn >= XENBUS_EVTCHN_INTERFACE_VERSION_MIN &&
                                                Debug >= XENBUS_DEBUG_INTERFACE_VERSION_MIN &&
                                                Store >= XENBUS_STORE_INTERFACE_VERSION_MIN &&
                                                RangeSet >= XENBUS_RANGE_SET_INTERFACE_VERSION_MIN &&
                                                Cache >= XENBUS_CACHE_INTERFACE_VERSION_MIN &&
                                                Gnttab >= XENBUS_GNTTAB_INTERFACE_VERSION_MIN &&
                                                Emulated >= XENFILT_EMULATED_INTERFACE_VERSION_MIN &&
                                                Unplug >= XENFILT_UNPLUG_INTERFACE_VERSION_MIN) {
                                                status = PdoAddRevision(Pdo, Revision,
                                                                        Suspend,
                                                                        SharedInfo,
                                                                        Evtchn,
                                                                        Debug,
                                                                        Store,
                                                                        RangeSet,
                                                                        Cache,
                                                                        Gnttab,
                                                                        Emulated,
                                                                        Unplug);
                                                if (!NT_SUCCESS(status))
                                                    goto fail1;
                                            }   
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }   
        }
    }                             

    ASSERT(Pdo->Count > 0);
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    if (Pdo->Description != NULL) {
        while (--Revision > 0)
            __PdoFree(Pdo->Description[Revision]);
        __PdoFree(Pdo->Description);
        Pdo->Description = NULL;
    }

    if (Pdo->Revision != NULL) {
        __PdoFree(Pdo->Revision);
        Pdo->Revision = NULL;
    }

    Pdo->Count = 0;

    return status;
}

static FORCEINLINE PDEVICE_OBJECT
__PdoGetDeviceObject(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;

    return (Dx->DeviceObject);
}
    
PDEVICE_OBJECT
PdoGetDeviceObject(
    IN  PXENBUS_PDO Pdo
    )
{
    return __PdoGetDeviceObject(Pdo);
}

static FORCEINLINE PCHAR
__PdoGetVendorName(
    IN  PXENBUS_PDO Pdo
    )
{
    return FdoGetVendorName(__PdoGetFdo(Pdo));
}

PDMA_ADAPTER
PdoGetDmaAdapter(
    IN  PXENBUS_PDO         Pdo,
    IN  PDEVICE_DESCRIPTION DeviceDescriptor,
    OUT PULONG              NumberOfMapRegisters
    )
{
    Trace("<===>\n");

    return FdoGetDmaAdapter(__PdoGetFdo(Pdo),
                            DeviceDescriptor,
                            NumberOfMapRegisters);
}

BOOLEAN
PdoTranslateBusAddress(
    IN      PXENBUS_PDO         Pdo,
    IN      PHYSICAL_ADDRESS    BusAddress,
    IN      ULONG               Length,
    IN OUT  PULONG              AddressSpace,
    OUT     PPHYSICAL_ADDRESS   TranslatedAddress
    )
{
    Trace("<===>\n");

    return FdoTranslateBusAddress(__PdoGetFdo(Pdo),
                                  BusAddress,
                                  Length,
                                  AddressSpace,
                                  TranslatedAddress);
}

ULONG
PdoSetBusData(
    IN  PXENBUS_PDO     Pdo,
    IN  ULONG           DataType,
    IN  PVOID           Buffer,
    IN  ULONG           Offset,
    IN  ULONG           Length
    )
{
    Trace("<===>\n");

    return FdoSetBusData(__PdoGetFdo(Pdo),
                         DataType,
                         Buffer,
                         Offset,
                         Length);
}

ULONG
PdoGetBusData(
    IN  PXENBUS_PDO     Pdo,
    IN  ULONG           DataType,
    IN  PVOID           Buffer,
    IN  ULONG           Offset,
    IN  ULONG           Length
    )
{
    Trace("<===>\n");

    return FdoGetBusData(__PdoGetFdo(Pdo),
                         DataType,
                         Buffer,
                         Offset,
                         Length);
}

static FORCEINLINE VOID
__PdoD3ToD0(
    IN  PXENBUS_PDO     Pdo
    )
{
    POWER_STATE         PowerState;

    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);
    ASSERT3U(__PdoGetDevicePowerState(Pdo), ==, PowerDeviceD3);

    __PdoSetDevicePowerState(Pdo, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

static FORCEINLINE VOID
__PdoD0ToD3(
    IN  PXENBUS_PDO     Pdo
    )
{
    POWER_STATE         PowerState;

    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);
    ASSERT3U(__PdoGetDevicePowerState(Pdo), ==, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    __PdoSetDevicePowerState(Pdo, PowerDeviceD3);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

static VOID
PdoSuspendCallbackLate(
    IN  PVOID   Argument
    )
{
    PXENBUS_PDO Pdo = Argument;

    __PdoD0ToD3(Pdo);
    __PdoD3ToD0(Pdo);
}

// This function must not touch pageable code or data
static NTSTATUS
PdoD3ToD0(
    IN  PXENBUS_PDO Pdo
    )
{
    KIRQL           Irql;
    NTSTATUS        status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = XENBUS_SUSPEND(Acquire, &Pdo->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoD3ToD0(Pdo);

    status = XENBUS_SUSPEND(Register,
                            &Pdo->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            PdoSuspendCallbackLate,
                            Pdo,
                            &Pdo->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail2;

    KeLowerIrql(Irql);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    __PdoD0ToD3(Pdo);

    XENBUS_SUSPEND(Release, &Pdo->SuspendInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

// This function must not touch pageable code or data
static VOID
PdoD0ToD3(
    IN  PXENBUS_PDO Pdo
    )
{
    KIRQL           Irql;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    XENBUS_SUSPEND(Deregister,
                   &Pdo->SuspendInterface,
                   Pdo->SuspendCallbackLate);
    Pdo->SuspendCallbackLate = NULL;

    __PdoD0ToD3(Pdo);

    XENBUS_SUSPEND(Release, &Pdo->SuspendInterface);

    KeLowerIrql(Irql);
}

// This function must not touch pageable code or data
static VOID
PdoS4ToS3(
    IN  PXENBUS_PDO Pdo
    )
{
    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__PdoGetSystemPowerState(Pdo), ==, PowerSystemHibernate);

    __PdoSetSystemPowerState(Pdo, PowerSystemSleeping3);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

// This function must not touch pageable code or data
static VOID
PdoS3ToS4(
    IN  PXENBUS_PDO Pdo
    )
{
    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__PdoGetSystemPowerState(Pdo), ==, PowerSystemSleeping3);

    __PdoSetSystemPowerState(Pdo, PowerSystemHibernate);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

static VOID
PdoParseResources(
    IN  PXENBUS_PDO             Pdo,
    IN  PCM_RESOURCE_LIST       RawResourceList,
    IN  PCM_RESOURCE_LIST       TranslatedResourceList
    )
{
    PCM_PARTIAL_RESOURCE_LIST   RawPartialList;
    PCM_PARTIAL_RESOURCE_LIST   TranslatedPartialList;
    ULONG                       Index;

    UNREFERENCED_PARAMETER(Pdo);

    ASSERT3U(RawResourceList->Count, ==, 1);
    RawPartialList = &RawResourceList->List[0].PartialResourceList;

    ASSERT3U(RawPartialList->Version, ==, 1);
    ASSERT3U(RawPartialList->Revision, ==, 1);

    ASSERT3U(TranslatedResourceList->Count, ==, 1);
    TranslatedPartialList = &TranslatedResourceList->List[0].PartialResourceList;

    ASSERT3U(TranslatedPartialList->Version, ==, 1);
    ASSERT3U(TranslatedPartialList->Revision, ==, 1);

    for (Index = 0; Index < TranslatedPartialList->Count; Index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR RawPartialDescriptor;
        PCM_PARTIAL_RESOURCE_DESCRIPTOR TranslatedPartialDescriptor;

        RawPartialDescriptor = &RawPartialList->PartialDescriptors[Index];
        TranslatedPartialDescriptor = &TranslatedPartialList->PartialDescriptors[Index];

        Trace("%s: [%d] %02x:%s\n",
              __PdoGetName(Pdo),
              Index,
              TranslatedPartialDescriptor->Type,
              ResourceDescriptorTypeName(TranslatedPartialDescriptor->Type));

        switch (TranslatedPartialDescriptor->Type) {
        case CmResourceTypeMemory:
            Trace("RAW: SharedDisposition=%02x Flags=%04x Start = %08x.%08x Length = %08x\n",
                  RawPartialDescriptor->ShareDisposition,
                  RawPartialDescriptor->Flags,
                  RawPartialDescriptor->u.Memory.Start.HighPart,
                  RawPartialDescriptor->u.Memory.Start.LowPart,
                  RawPartialDescriptor->u.Memory.Length);

            Trace("TRANSLATED: SharedDisposition=%02x Flags=%04x Start = %08x.%08x Length = %08x\n",
                  TranslatedPartialDescriptor->ShareDisposition,
                  TranslatedPartialDescriptor->Flags,
                  TranslatedPartialDescriptor->u.Memory.Start.HighPart,
                  TranslatedPartialDescriptor->u.Memory.Start.LowPart,
                  TranslatedPartialDescriptor->u.Memory.Length);
            break;

        case CmResourceTypeInterrupt:
            Trace("RAW: SharedDisposition=%02x Flags=%04x Level = %08x Vector = %08x Affinity = %p\n",
                  RawPartialDescriptor->ShareDisposition,
                  RawPartialDescriptor->Flags,
                  RawPartialDescriptor->u.Interrupt.Level,
                  RawPartialDescriptor->u.Interrupt.Vector,
                  (PVOID)RawPartialDescriptor->u.Interrupt.Affinity);

            Trace("TRANSLATED: SharedDisposition=%02x Flags=%04x Level = %08x Vector = %08x Affinity = %p\n",
                  TranslatedPartialDescriptor->ShareDisposition,
                  TranslatedPartialDescriptor->Flags,
                  TranslatedPartialDescriptor->u.Interrupt.Level,
                  TranslatedPartialDescriptor->u.Interrupt.Vector,
                  (PVOID)TranslatedPartialDescriptor->u.Interrupt.Affinity);
            break;

        default:
            break;
        }
    }

    Trace("<====\n");
}

static NTSTATUS
PdoStartDevice(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    PdoParseResources(Pdo,
                      StackLocation->Parameters.StartDevice.AllocatedResources,
                      StackLocation->Parameters.StartDevice.AllocatedResourcesTranslated);

    PdoD3ToD0(Pdo);

    __PdoSetDevicePnpState(Pdo, Started);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoQueryStopDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __PdoSetDevicePnpState(Pdo, StopPending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoCancelStopDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __PdoRestoreDevicePnpState(Pdo, StopPending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoStopDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    PdoD0ToD3(Pdo);

    __PdoSetDevicePnpState(Pdo, Stopped);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryRemoveDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __PdoSetDevicePnpState(Pdo, RemovePending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoCancelRemoveDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    __PdoRestoreDevicePnpState(Pdo, RemovePending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoSurpriseRemoval(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    Warning("%s\n", __PdoGetName(Pdo));

    __PdoSetDevicePnpState(Pdo, SurpriseRemovePending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoRemoveDevice(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    PXENBUS_FDO     Fdo = __PdoGetFdo(Pdo);
    NTSTATUS        status;

    if (__PdoGetDevicePowerState(Pdo) != PowerDeviceD0)
        goto done;

    PdoD0ToD3(Pdo);

done:
    FdoAcquireMutex(Fdo);

    if (__PdoIsMissing(Pdo) ||
        __PdoGetDevicePnpState(Pdo) == SurpriseRemovePending)
        __PdoSetDevicePnpState(Pdo, Deleted);
    else
        __PdoSetDevicePnpState(Pdo, Enumerated);

    if (__PdoIsMissing(Pdo)) {
        if (__PdoGetDevicePnpState(Pdo) == Deleted)
            PdoDestroy(Pdo);
        else
            IoInvalidateDeviceRelations(FdoGetPhysicalDeviceObject(Fdo), 
                                        BusRelations);
    }

    FdoReleaseMutex(Fdo);

    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryDeviceRelations(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    PDEVICE_RELATIONS   Relations;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    status = Irp->IoStatus.Status;

    if (StackLocation->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation)
        goto done;

    Relations = __AllocatePoolWithTag(PagedPool, sizeof (DEVICE_RELATIONS), 'SUB');

    status = STATUS_NO_MEMORY;
    if (Relations == NULL)
        goto done;

    Relations->Count = 1;
    ObReferenceObject(__PdoGetDeviceObject(Pdo));
    Relations->Objects[0] = __PdoGetDeviceObject(Pdo);

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoDelegateIrp(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    return FdoDelegateIrp(__PdoGetFdo(Pdo), Irp);
}

static NTSTATUS
PdoDelegateIrp(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    return __PdoDelegateIrp(Pdo, Irp);
}

static NTSTATUS
PdoQueryBusInterface(
    IN  PXENBUS_PDO         Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    USHORT                  Size;
    USHORT                  Version;
    PBUS_INTERFACE_STANDARD BusInterface;
    NTSTATUS                status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    BusInterface = (PBUS_INTERFACE_STANDARD)StackLocation->Parameters.QueryInterface.Interface;

    if (Version != 1)
        goto done;

    status = STATUS_BUFFER_TOO_SMALL;        
    if (Size < sizeof (BUS_INTERFACE_STANDARD))
        goto done;

    *BusInterface = Pdo->BusInterface;
    BusInterface->InterfaceReference(BusInterface->Context);

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

#define DEFINE_PDO_QUERY_INTERFACE(_Interface)                      \
static NTSTATUS                                                     \
PdoQuery ## _Interface ## Interface(                                \
    IN  PXENBUS_PDO     Pdo,                                        \
    IN  PIRP            Irp                                         \
    )                                                               \
{                                                                   \
    PIO_STACK_LOCATION  StackLocation;                              \
    USHORT              Size;                                       \
    USHORT              Version;                                    \
    PINTERFACE          Interface;                                  \
    PVOID               Context;                                    \
    NTSTATUS            status;                                     \
                                                                    \
    status = Irp->IoStatus.Status;                                  \
                                                                    \
    StackLocation = IoGetCurrentIrpStackLocation(Irp);              \
    Size = StackLocation->Parameters.QueryInterface.Size;           \
    Version = StackLocation->Parameters.QueryInterface.Version;     \
    Interface = StackLocation->Parameters.QueryInterface.Interface; \
                                                                    \
    Context = FdoGet ## _Interface ## Context(__PdoGetFdo(Pdo));    \
                                                                    \
    status = _Interface ## GetInterface(Context,                    \
                                        Version,                    \
                                        Interface,                  \
                                        Size);                      \
    if (!NT_SUCCESS(status))                                        \
        goto done;                                                  \
                                                                    \
    Irp->IoStatus.Information = 0;                                  \
    status = STATUS_SUCCESS;                                        \
                                                                    \
done:                                                               \
    return status;                                                  \
}                                                                   \

DEFINE_PDO_QUERY_INTERFACE(Debug)
DEFINE_PDO_QUERY_INTERFACE(Suspend)
DEFINE_PDO_QUERY_INTERFACE(SharedInfo)
DEFINE_PDO_QUERY_INTERFACE(Evtchn)
DEFINE_PDO_QUERY_INTERFACE(Store)
DEFINE_PDO_QUERY_INTERFACE(RangeSet)
DEFINE_PDO_QUERY_INTERFACE(Cache)
DEFINE_PDO_QUERY_INTERFACE(Gnttab)

struct _INTERFACE_ENTRY {
    const GUID  *Guid;
    const CHAR  *Name;
    NTSTATUS    (*Query)(PXENBUS_PDO, PIRP);
};

static struct _INTERFACE_ENTRY PdoInterfaceTable[] = {
    { &GUID_BUS_INTERFACE_STANDARD, "BUS_INTERFACE", PdoQueryBusInterface },
    { &GUID_XENBUS_DEBUG_INTERFACE, "DEBUG_INTERFACE", PdoQueryDebugInterface },
    { &GUID_XENBUS_SUSPEND_INTERFACE, "SUSPEND_INTERFACE", PdoQuerySuspendInterface },
    { &GUID_XENBUS_SHARED_INFO_INTERFACE, "SHARED_INFO_INTERFACE", PdoQuerySharedInfoInterface },
    { &GUID_XENBUS_EVTCHN_INTERFACE, "EVTCHN_INTERFACE", PdoQueryEvtchnInterface },
    { &GUID_XENBUS_STORE_INTERFACE, "STORE_INTERFACE", PdoQueryStoreInterface },
    { &GUID_XENBUS_RANGE_SET_INTERFACE, "RANGE_SET_INTERFACE", PdoQueryRangeSetInterface },
    { &GUID_XENBUS_CACHE_INTERFACE, "CACHE_INTERFACE", PdoQueryCacheInterface },
    { &GUID_XENBUS_GNTTAB_INTERFACE, "GNTTAB_INTERFACE", PdoQueryGnttabInterface },
    { &GUID_XENFILT_EMULATED_INTERFACE, "EMULATED_INTERFACE", PdoDelegateIrp },
    { &GUID_XENFILT_UNPLUG_INTERFACE, "UNPLUG_INTERFACE", PdoDelegateIrp },
    { NULL, NULL, NULL }
};

static NTSTATUS
PdoQueryInterface(
    IN  PXENBUS_PDO         Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    const GUID              *InterfaceType;
    struct _INTERFACE_ENTRY *Entry;
    USHORT                  Version;
    NTSTATUS                status;

    status = Irp->IoStatus.Status;

    if (status != STATUS_NOT_SUPPORTED)
        goto done;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    InterfaceType = StackLocation->Parameters.QueryInterface.InterfaceType;
    Version = StackLocation->Parameters.QueryInterface.Version;

    for (Entry = PdoInterfaceTable; Entry->Guid != NULL; Entry++) {
        if (IsEqualGUID(InterfaceType, Entry->Guid)) {
            Info("%s: %s (VERSION %d)\n",
                 __PdoGetName(Pdo),
                 Entry->Name,
                 Version);
            status = Entry->Query(Pdo, Irp);
            goto done;
        }
    }

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryCapabilities(
    IN  PXENBUS_PDO         Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    PDEVICE_CAPABILITIES    Capabilities;
    SYSTEM_POWER_STATE      SystemPowerState;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Pdo);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Capabilities = StackLocation->Parameters.DeviceCapabilities.Capabilities;

    status = STATUS_INVALID_PARAMETER;
    if (Capabilities->Version != 1)
        goto done;

    Capabilities->DeviceD1 = 0;
    Capabilities->DeviceD2 = 0;
    Capabilities->LockSupported = 0;
    Capabilities->DockDevice = 0;
    Capabilities->UniqueID = 1;
    Capabilities->SilentInstall = 1;
    Capabilities->RawDeviceOK = 0;
    Capabilities->HardwareDisabled = 0;
    Capabilities->NoDisplayInUI = 0;

    Capabilities->Removable = __PdoIsRemovable(Pdo) ? 1 : 0;
    Capabilities->SurpriseRemovalOK = Capabilities->Removable;
    Capabilities->EjectSupported = __PdoIsEjectable(Pdo) ? 1 : 0;

    Capabilities->Address = 0xffffffff;
    Capabilities->UINumber = 0xffffffff;

    for (SystemPowerState = 0; SystemPowerState < PowerSystemMaximum; SystemPowerState++) {
        switch (SystemPowerState) {
        case PowerSystemUnspecified:
        case PowerSystemSleeping1:
        case PowerSystemSleeping2:
            break;

        case PowerSystemWorking:
            Capabilities->DeviceState[SystemPowerState] = PowerDeviceD0;
            break;

        default:
            Capabilities->DeviceState[SystemPowerState] = PowerDeviceD3;
            break;
        }
    }

    Capabilities->SystemWake = PowerSystemUnspecified;
    Capabilities->DeviceWake = PowerDeviceUnspecified;
    Capabilities->D1Latency = 0;
    Capabilities->D2Latency = 0;
    Capabilities->D3Latency = 0;

    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryResourceRequirements(
    IN  PXENBUS_PDO                 Pdo,
    IN  PIRP                        Irp
    )
{
    IO_RESOURCE_DESCRIPTOR          Memory;
    IO_RESOURCE_DESCRIPTOR          Interrupt;
    ULONG                           Size;
    PIO_RESOURCE_REQUIREMENTS_LIST  Requirements;
    PIO_RESOURCE_LIST               List;
    NTSTATUS                        status;

    UNREFERENCED_PARAMETER(Pdo);

    RtlZeroMemory(&Memory, sizeof (IO_RESOURCE_DESCRIPTOR));
    Memory.Type = CmResourceTypeMemory;
    Memory.ShareDisposition = CmResourceShareDeviceExclusive;
    Memory.Flags = CM_RESOURCE_MEMORY_READ_WRITE |
                   CM_RESOURCE_MEMORY_PREFETCHABLE |
                   CM_RESOURCE_MEMORY_CACHEABLE;

    Memory.u.Memory.Length = PAGE_SIZE;
    Memory.u.Memory.Alignment = PAGE_SIZE;
    Memory.u.Memory.MinimumAddress.QuadPart = 0;
    Memory.u.Memory.MaximumAddress.QuadPart = -1;

    RtlZeroMemory(&Interrupt, sizeof (IO_RESOURCE_DESCRIPTOR));
    Interrupt.Type = CmResourceTypeInterrupt;
    Interrupt.ShareDisposition = CmResourceShareDeviceExclusive;
    Interrupt.Flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;

    Interrupt.u.Interrupt.MinimumVector = (ULONG)0;
    Interrupt.u.Interrupt.MaximumVector = (ULONG)-1;
    Interrupt.u.Interrupt.AffinityPolicy = IrqPolicyOneCloseProcessor;
    Interrupt.u.Interrupt.PriorityPolicy = IrqPriorityUndefined;

    Size = sizeof (IO_RESOURCE_DESCRIPTOR) * 2;
    Size += FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors);
    Size += FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List);

    Requirements = __AllocatePoolWithTag(PagedPool, Size, 'SUB');

    status = STATUS_NO_MEMORY;
    if (Requirements == NULL)
        goto fail1;

    Requirements->ListSize = Size;
    Requirements->InterfaceType = Internal;
    Requirements->BusNumber = 0;
    Requirements->SlotNumber = 0;
    Requirements->AlternativeLists = 1;

    List = &Requirements->List[0];
    List->Version = 1;
    List->Revision = 1;
    List->Count = 2;
    List->Descriptors[0] = Memory;
    List->Descriptors[1] = Interrupt;

    Irp->IoStatus.Information = (ULONG_PTR)Requirements;

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryDeviceText(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    PWCHAR              Buffer;
    UNICODE_STRING      Text;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->Parameters.QueryDeviceText.DeviceTextType) {
    case DeviceTextDescription:
        Trace("DeviceTextDescription\n");
        break;

    case DeviceTextLocationInformation:
        Trace("DeviceTextLocationInformation\n");
        break;

    default:
        Irp->IoStatus.Information = 0;
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }

    Buffer = __AllocatePoolWithTag(PagedPool, MAXTEXTLEN, 'SUB');

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto done;

    Text.Buffer = Buffer;
    Text.MaximumLength = MAXTEXTLEN;
    Text.Length = 0;

    switch (StackLocation->Parameters.QueryDeviceText.DeviceTextType) {
    case DeviceTextDescription: {
        ULONG   Index = Pdo->Count - 1;

        status = RtlStringCbPrintfW(Buffer,
                                    MAXTEXTLEN,
                                    L"%s",
                                    Pdo->Description[Index]);
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;
    }
    case DeviceTextLocationInformation:
        status = RtlStringCbPrintfW(Buffer,
                                    MAXTEXTLEN,
                                    L"%hs",
                                    __PdoGetName(Pdo));
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;

    default:
        ASSERT(FALSE);
        break;
    }

    Text.Length = (USHORT)((ULONG_PTR)Buffer - (ULONG_PTR)Text.Buffer);

    Trace("%s: %wZ\n", __PdoGetName(Pdo), &Text);

    Irp->IoStatus.Information = (ULONG_PTR)Text.Buffer;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoReadConfig(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    UNREFERENCED_PARAMETER(Pdo);

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS
PdoWriteConfig(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    UNREFERENCED_PARAMETER(Pdo);

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_NOT_SUPPORTED;
}

#define MAX_DEVICE_ID_LEN   200

static NTSTATUS
PdoQueryId(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    PWCHAR              Buffer;
    UNICODE_STRING      Id;
    ULONG               Type;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->Parameters.QueryId.IdType) {
    case BusQueryInstanceID:
        Trace("BusQueryInstanceID\n");
        Id.MaximumLength = 2 * sizeof (WCHAR);
        break;

    case BusQueryDeviceID:
        Trace("BusQueryDeviceID\n");
        Id.MaximumLength = (MAX_DEVICE_ID_LEN - 2) * sizeof (WCHAR);
        break;

    case BusQueryHardwareIDs:
        Trace("BusQueryHardwareIDs\n");
        Id.MaximumLength = (USHORT)(MAX_DEVICE_ID_LEN * Pdo->Count) * sizeof (WCHAR);
        break;

    case BusQueryCompatibleIDs:
        Trace("BusQueryCompatibleIDs\n");
        Id.MaximumLength = (USHORT)(MAX_DEVICE_ID_LEN * Pdo->Count) * sizeof (WCHAR);
        break;

    default:
        Irp->IoStatus.Information = 0;
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }

    Buffer = __AllocatePoolWithTag(PagedPool, Id.MaximumLength, 'SUB');

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto done;

    Id.Buffer = Buffer;
    Id.Length = 0;

    switch (StackLocation->Parameters.QueryId.IdType) {
    case BusQueryInstanceID:
        Type = REG_SZ;

        status = RtlAppendUnicodeToString(&Id, L"_");
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;

    case BusQueryDeviceID: {
        ULONG   Index;

        Type = REG_SZ;
        Index = Pdo->Count - 1;

        status = RtlStringCbPrintfW(Buffer,
                                    Id.MaximumLength,
                                    L"XENBUS\\VEN_%hs&DEV_%hs&REV_%08X",
                                    __PdoGetVendorName(Pdo),
                                    __PdoGetName(Pdo),
                                    Pdo->Revision[Index]);
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;
    }
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs: {
        ULONG   Index;
        ULONG   Length;

        Type = REG_MULTI_SZ;
        Length = Id.MaximumLength;

        for (Index = 0; Index < Pdo->Count; Index++) {
            status = RtlStringCbPrintfW(Buffer,
                                        Length,
                                        L"XENBUS\\VEN_%hs&DEV_%hs&REV_%08X",
                                        __PdoGetVendorName(Pdo),
                                        __PdoGetName(Pdo),
                                        Pdo->Revision[Index]);
            ASSERT(NT_SUCCESS(status));

            Buffer += wcslen(Buffer);
            Length -= (ULONG)(wcslen(Buffer) * sizeof (WCHAR));

            Buffer++;
            Length -= sizeof (WCHAR);
        }

        status = RtlStringCbPrintfW(Buffer,
                                    Length,
                                    L"XENCLASS");
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);
        Buffer++;

        break;
    }
    default:
        Type = REG_NONE;

        ASSERT(FALSE);
        break;
    }

    Id.Length = (USHORT)((ULONG_PTR)Buffer - (ULONG_PTR)Id.Buffer);
    Buffer = Id.Buffer;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    switch (Type) {
    case REG_SZ:
        Trace("- %ws\n", Buffer);
        break;

    case REG_MULTI_SZ:
        do {
            Trace("- %ws\n", Buffer);
            Buffer += wcslen(Buffer);
            Buffer++;
        } while (*Buffer != L'\0');
        break;

    default:
        ASSERT(FALSE);
        break;
    }

    Irp->IoStatus.Information = (ULONG_PTR)Id.Buffer;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoQueryBusInformation(
    IN  PXENBUS_PDO         Pdo,
    IN  PIRP                Irp
    )
{
    PPNP_BUS_INFORMATION    Info;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Pdo);

    Info = __AllocatePoolWithTag(PagedPool, sizeof (PNP_BUS_INFORMATION), 'SUB');

    status = STATUS_NO_MEMORY;
    if (Info == NULL)
        goto done;

    Info->BusTypeGuid = GUID_BUS_TYPE_INTERNAL;
    Info->LegacyBusType = Internal;
    Info->BusNumber = 0;

    Irp->IoStatus.Information = (ULONG_PTR)Info;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoDeviceUsageNotification(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    status = __PdoDelegateIrp(Pdo, Irp);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoEject(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    PXENBUS_FDO     Fdo = __PdoGetFdo(Pdo);
    NTSTATUS        status;

    Trace("%s\n", __PdoGetName(Pdo));

    FdoAcquireMutex(Fdo);

    __PdoSetDevicePnpState(Pdo, Deleted);
    __PdoSetMissing(Pdo, "device ejected");

    PdoDestroy(Pdo);

    FdoReleaseMutex(Fdo);

    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoDispatchPnp(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    Trace("====> (%02x:%s)\n",
          MinorFunction, 
          PnpMinorFunctionName(MinorFunction));

    switch (StackLocation->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = PdoStartDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
        status = PdoQueryStopDevice(Pdo, Irp);
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:
        status = PdoCancelStopDevice(Pdo, Irp);
        break;

    case IRP_MN_STOP_DEVICE:
        status = PdoStopDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
        status = PdoQueryRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = PdoCancelRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        status = PdoSurpriseRemoval(Pdo, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = PdoRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        status = PdoQueryDeviceRelations(Pdo, Irp);
        break;

    case IRP_MN_QUERY_INTERFACE:
        status = PdoQueryInterface(Pdo, Irp);
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        status = PdoQueryCapabilities(Pdo, Irp);
        break;

    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        status = PdoQueryResourceRequirements(Pdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_TEXT:
        status = PdoQueryDeviceText(Pdo, Irp);
        break;

    case IRP_MN_READ_CONFIG:
        status = PdoReadConfig(Pdo, Irp);
        break;

    case IRP_MN_WRITE_CONFIG:
        status = PdoWriteConfig(Pdo, Irp);
        break;

    case IRP_MN_QUERY_ID:
        status = PdoQueryId(Pdo, Irp);
        break;

    case IRP_MN_QUERY_BUS_INFORMATION:
        status = PdoQueryBusInformation(Pdo, Irp);
        break;

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        status = PdoDeviceUsageNotification(Pdo, Irp);
        break;

    case IRP_MN_EJECT:
        status = PdoEject(Pdo, Irp);
        break;

    default:
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

    Trace("<==== (%02x:%s)(%08x)\n",
          MinorFunction, 
          PnpMinorFunctionName(MinorFunction),
          status);

    return status;
}

static NTSTATUS
PdoSetDevicePower(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    POWER_ACTION        PowerAction;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          DevicePowerStateName(DeviceState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <, PowerActionShutdown);

    if (__PdoGetDevicePowerState(Pdo) > DeviceState) {
        Trace("%s: POWERING UP: %s -> %s\n",
              __PdoGetName(Pdo),
              DevicePowerStateName(__PdoGetDevicePowerState(Pdo)),
              DevicePowerStateName(DeviceState));

        ASSERT3U(DeviceState, ==, PowerDeviceD0);
        PdoD3ToD0(Pdo);
    } else if (__PdoGetDevicePowerState(Pdo) < DeviceState) {
        Trace("%s: POWERING DOWN: %s -> %s\n",
              __PdoGetName(Pdo),
              DevicePowerStateName(__PdoGetDevicePowerState(Pdo)),
              DevicePowerStateName(DeviceState));

        ASSERT3U(DeviceState, ==, PowerDeviceD3);
        PdoD0ToD3(Pdo);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    Trace("<==== (%s:%s)\n",
          DevicePowerStateName(DeviceState), 
          PowerActionName(PowerAction));

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoDevicePower(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_PDO         Pdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP    Irp;

        if (Pdo->DevicePowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Pdo->DevicePowerIrp;

        if (Irp == NULL)
            continue;

        Pdo->DevicePowerIrp = NULL;
        KeMemoryBarrier();

        (VOID) PdoSetDevicePower(Pdo, Irp);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoSetSystemPower(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    SYSTEM_POWER_STATE  SystemState;
    POWER_ACTION        PowerAction;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s:%s)\n",
          SystemPowerStateName(SystemState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <, PowerActionShutdown);

    if (__PdoGetSystemPowerState(Pdo) > SystemState) {
        if (SystemState < PowerSystemHibernate &&
            __PdoGetSystemPowerState(Pdo) >= PowerSystemHibernate) {
            __PdoSetSystemPowerState(Pdo, PowerSystemHibernate);
            PdoS4ToS3(Pdo);
        }

        Trace("%s: POWERING UP: %s -> %s\n",
              __PdoGetName(Pdo),
              SystemPowerStateName(__PdoGetSystemPowerState(Pdo)),
              SystemPowerStateName(SystemState));

    } else if (__PdoGetSystemPowerState(Pdo) < SystemState) {
        Trace("%s: POWERING DOWN: %s -> %s\n",
              __PdoGetName(Pdo),
              SystemPowerStateName(__PdoGetSystemPowerState(Pdo)),
              SystemPowerStateName(SystemState));

        if (SystemState >= PowerSystemHibernate &&
            __PdoGetSystemPowerState(Pdo) < PowerSystemHibernate) {
            __PdoSetSystemPowerState(Pdo, PowerSystemSleeping3);
            PdoS3ToS4(Pdo);
        }
    }

    __PdoSetSystemPowerState(Pdo, SystemState);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    Trace("<==== (%s:%s)\n",
          SystemPowerStateName(SystemState), 
          PowerActionName(PowerAction));

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoSystemPower(
    IN  PXENBUS_THREAD  Self,
    IN  PVOID           Context
    )
{
    PXENBUS_PDO         Pdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP    Irp;

        if (Pdo->SystemPowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Pdo->SystemPowerIrp;

        if (Irp == NULL)
            continue;

        Pdo->SystemPowerIrp = NULL;
        KeMemoryBarrier();

        (VOID) PdoSetSystemPower(Pdo, Irp);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoSetPower(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    POWER_STATE_TYPE    PowerType;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;
    
    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    PowerType = StackLocation->Parameters.Power.Type;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    if (PowerAction >= PowerActionShutdown) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    switch (PowerType) {
    case DevicePowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Pdo->DevicePowerIrp, ==, NULL);
        Pdo->DevicePowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Pdo->DevicePowerThread);

        status = STATUS_PENDING;
        break;

    case SystemPowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Pdo->SystemPowerIrp, ==, NULL);
        Pdo->SystemPowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Pdo->SystemPowerThread);

        status = STATUS_PENDING;
        break;

    default:
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

done:
    return status;
}

static NTSTATUS
PdoQueryPower(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    UNREFERENCED_PARAMETER(Pdo);

    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoDispatchPower(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    switch (StackLocation->MinorFunction) {
    case IRP_MN_SET_POWER:
        status = PdoSetPower(Pdo, Irp);
        break;

    case IRP_MN_QUERY_POWER:
        status = PdoQueryPower(Pdo, Irp);
        break;

    default:
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

    return status;
}

static NTSTATUS
PdoDispatchDefault(
    IN  PXENBUS_PDO Pdo,
    IN  PIRP        Irp
    )
{
    NTSTATUS        status;

    UNREFERENCED_PARAMETER(Pdo);

    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

NTSTATUS
PdoDispatch(
    IN  PXENBUS_PDO     Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_PNP:
        status = PdoDispatchPnp(Pdo, Irp);
        break;

    case IRP_MJ_POWER:
        status = PdoDispatchPower(Pdo, Irp);
        break;

    default:
        status = PdoDispatchDefault(Pdo, Irp);
        break;
    }

    return status;
}

VOID
PdoResume(
    IN  PXENBUS_PDO     Pdo
    )
{
    UNREFERENCED_PARAMETER(Pdo);

    Trace("<===>\n");
}

VOID
PdoSuspend(
    IN  PXENBUS_PDO     Pdo
    )
{
    UNREFERENCED_PARAMETER(Pdo);

    Trace("<===>\n");
}

NTSTATUS
PdoCreate(
    IN  PXENBUS_FDO     Fdo,
    IN  PANSI_STRING    Name
    )
{
    PDEVICE_OBJECT      PhysicalDeviceObject;
    PXENBUS_DX          Dx;
    PXENBUS_PDO         Pdo;
    ULONG               Index;
    NTSTATUS            status;

#pragma prefast(suppress:28197) // Possibly leaking memory 'PhysicalDeviceObject'
    status = IoCreateDevice(DriverGetDriverObject(),
                            sizeof(XENBUS_DX),
                            NULL,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN | FILE_AUTOGENERATED_DEVICE_NAME,
                            FALSE,
                            &PhysicalDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENBUS_DX)PhysicalDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENBUS_DX));

    Dx->Type = PHYSICAL_DEVICE_OBJECT;
    Dx->DeviceObject = PhysicalDeviceObject;
    Dx->DevicePnpState = Present;

    Dx->SystemPowerState = PowerSystemWorking;
    Dx->DevicePowerState = PowerDeviceD3;

    Pdo = __PdoAllocate(sizeof (XENBUS_PDO));

    status = STATUS_NO_MEMORY;
    if (Pdo == NULL)
        goto fail2;

    Pdo->Dx = Dx;
    Pdo->Fdo = Fdo;

    status = ThreadCreate(PdoSystemPower, Pdo, &Pdo->SystemPowerThread);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = ThreadCreate(PdoDevicePower, Pdo, &Pdo->DevicePowerThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    __PdoSetName(Pdo, Name);
    __PdoSetRemovable(Pdo);
    __PdoSetEjectable(Pdo);

    status = PdoSetRevisions(Pdo);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = BusInitialize(Pdo, &Pdo->BusInterface);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = SuspendGetInterface(FdoGetSuspendContext(Fdo),
                                 XENBUS_SUSPEND_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&Pdo->SuspendInterface,
                                 sizeof (Pdo->SuspendInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT(Pdo->SuspendInterface.Interface.Context != NULL);

    for (Index = 0; Index < Pdo->Count; Index++) {
        Info("%p (%s %08X)\n",
             PhysicalDeviceObject,
             __PdoGetName(Pdo),
             Pdo->Revision[Index]);
    }

    Dx->Pdo = Pdo;
    PhysicalDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    FdoAddPhysicalDeviceObject(Fdo, Pdo);

    return STATUS_SUCCESS;

fail6:
    Error("fail6\n");

    for (Index = 0; Index < Pdo->Count; Index++)
        __PdoFree(Pdo->Description[Index]);
    __PdoFree(Pdo->Description);
    Pdo->Description = NULL;

    __PdoFree(Pdo->Revision);
    Pdo->Revision = NULL;
    Pdo->Count = 0;

fail5:
    Error("fail5\n");

    Pdo->Removable = FALSE;

    ThreadAlert(Pdo->DevicePowerThread);
    ThreadJoin(Pdo->DevicePowerThread);
    Pdo->DevicePowerThread = NULL;

fail4:
    Error("fail4\n");

    ThreadAlert(Pdo->SystemPowerThread);
    ThreadJoin(Pdo->SystemPowerThread);
    Pdo->SystemPowerThread = NULL;

fail3:
    Error("fail3\n");

    Pdo->Fdo = NULL;
    Pdo->Dx = NULL;

    ASSERT(IsZeroMemory(Pdo, sizeof (XENBUS_PDO)));
    __PdoFree(Pdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(PhysicalDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
PdoDestroy(
    IN  PXENBUS_PDO Pdo
    )
{
    PXENBUS_DX      Dx = Pdo->Dx;
    PDEVICE_OBJECT  PhysicalDeviceObject = Dx->DeviceObject;
    PXENBUS_FDO     Fdo = __PdoGetFdo(Pdo);
    ULONG           Index;

    ASSERT3U(__PdoGetDevicePnpState(Pdo), ==, Deleted);

    ASSERT(__PdoIsMissing(Pdo));
    Pdo->Missing = FALSE;

    FdoRemovePhysicalDeviceObject(Fdo, Pdo);

    Info("%p (%s) (%s)\n",
         PhysicalDeviceObject,
         __PdoGetName(Pdo),
         Pdo->Reason);
    Pdo->Reason = NULL;

    Dx->Pdo = NULL;

    RtlZeroMemory(&Pdo->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    BusTeardown(&Pdo->BusInterface);

    for (Index = 0; Index < Pdo->Count; Index++)
        __PdoFree(Pdo->Description[Index]);
    __PdoFree(Pdo->Description);
    Pdo->Description = NULL;

    __PdoFree(Pdo->Revision);
    Pdo->Revision = NULL;
    Pdo->Count = 0;

    Pdo->Ejectable = FALSE;
    Pdo->Removable = FALSE;

    ThreadAlert(Pdo->DevicePowerThread);
    ThreadJoin(Pdo->DevicePowerThread);
    Pdo->DevicePowerThread = NULL;
    
    ThreadAlert(Pdo->SystemPowerThread);
    ThreadJoin(Pdo->SystemPowerThread);
    Pdo->SystemPowerThread = NULL;

    Pdo->Fdo = NULL;
    Pdo->Dx = NULL;

    ASSERT(IsZeroMemory(Pdo, sizeof (XENBUS_PDO)));
    __PdoFree(Pdo);

    IoDeleteDevice(PhysicalDeviceObject);
}
