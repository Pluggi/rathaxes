#include "driver.h"

#include <ntstrsafe.h>
#include <wdm.h>

#include "device.h"
#include "pnp.h"
#include "read.h"
#include "write.h"
#include "ioctl.h"
#include "uart.h"

void ReadThread(void* p)
{
	PMY_SERIAL_DEVICE_EXTENSION devExt;
	LARGE_INTEGER				time_to_wait;

	time_to_wait.QuadPart = -10000;
	devExt = (PMY_SERIAL_DEVICE_EXTENSION)p;

	while (devExt->mustExit == FALSE)
	{
		while (devExt->mustExit == FALSE && UartGetBit(devExt, LSR, LSR_DA) == 0)
		{
			  KeDelayExecutionThread(KernelMode, FALSE, &time_to_wait);
		}
		if (devExt->mustExit == FALSE)
		{
			WdfWaitLockAcquire(devExt->lock, NULL);
			if (devExt->readBufferLength >= MY_SERIAL_READ_BUFFER_MAX_LEN)
				devExt->readBufferLength = 0;
			devExt->readBuffer[devExt->readBufferLength++] = UartReadRegister(devExt, RBR);
			WdfWaitLockRelease(devExt->lock);
		}
	}
	PsTerminateSystemThread(STATUS_SUCCESS);
}

/*++
EvtDeviceAdd callback
--------------------
Appel�e lorsque le PnP manager d�tecte un p�riph�rique
qui correspond � un des ID mat�riel support� par le driver.
Ces IDs sont d�finis dans le .inf du driver.
--------------------
IRQL = PASSIVE_LEVEL
--*/
NTSTATUS
EvtDeviceAdd(
    IN WDFDRIVER       Driver,
    IN PWDFDEVICE_INIT DeviceInit
    )
{
	NTSTATUS						status;
	static ULONG					currentInstance = 0;
	WDF_PNPPOWER_EVENT_CALLBACKS	pnpPowerCallbacks;
	WDFDEVICE						device;
	WDF_IO_QUEUE_CONFIG				queueConfig;
    WDFQUEUE						defaultqueue;
	PULONG							countSoFar;
	WDF_OBJECT_ATTRIBUTES			attributes;
	PMY_SERIAL_DEVICE_EXTENSION		devExt;
	OBJECT_ATTRIBUTES				ObjectAttributes;

	/*++
	Declare et initialise une UNICODE_STRING
	--*/
	DECLARE_UNICODE_STRING_SIZE(deviceName, 20);
	DECLARE_UNICODE_STRING_SIZE(symbolicLinkName, 50);

    UNREFERENCED_PARAMETER(Driver);

	/*++
	La macro PAGED_CODE() indique que le code qui suit dans cette fonction
	peut etre pagin� (sur le disque, et non en m�moire physique)
	Permet au compilateur et aux outils de debug de d�tecter une erreur
	si le code tente d'apeller, par exemple, une fonction n�cessitant
	un acc�s � des donn�es situ�es obligatoirement en m�moire physique (pas de page_fault possible).
	--*/
	PAGED_CODE();

	KdPrint((DRIVER_NAME "--> EvtDeviceAdd\n"));

	/*++
	R�cupere le nombre actuel de ports s�rie enregistr�s
	sur le syst�me.
	--*/
	countSoFar = &IoGetConfigurationInformation()->SerialCount;

	/*++
	Construit une chaine de caractere unicode pour nommer le device.
	RtlUnicodeStringPrintf est d�finie dans <ntstrsafe.h>
	--*/
	status = RtlUnicodeStringPrintf(&deviceName, L"%ws%d",
                                L"\\Device\\Serial",
                                *countSoFar);
    if (!NT_SUCCESS(status))
	{
		KdPrint((DRIVER_NAME "Impossible de formater la string deviceName [0x%x]\n", status));
		return status;
	}
    status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
    if (!NT_SUCCESS(status))
	{
		KdPrint((DRIVER_NAME "Impossible d'assigner le nom de device [0x%x]\n", status));
        return status;
	}
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
	/*++
	WdfDeviceInitSetDeviceType indique le type de p�ripherique g�r�.
	L'appel � cette fonction se traduit par la modification du boost de priorit�
	appliqu� lors de la completion d'un requete d'I/O, ce qui permet d'augmenter
	la priorit� du thread qui a envoy� la requete.
	Les niveaux d'augmentation sont des constantes d�finies dans <wdm.h>
	--*/
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_SERIAL_PORT);
	KdPrint((DRIVER_NAME "Device nomm�, mode exclusif, FILE_DEVICE_SERIAL_PORT\n"));

	/*++
	Initialisation des callbacks PnP et gestion d'energie.
	Ces callbacks seront appel�es par le framework lors des ev�nements correspondants.
	--*/
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = EvtReleaseHardware;

	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	/*++
	Cr�ation du device en lui meme, en lui associant une structure de donn�es.
	--*/
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MY_SERIAL_DEVICE_EXTENSION);
	attributes.EvtCleanupCallback = EvtDeviceContextCleanup;

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
	{
        KdPrint((DRIVER_NAME "WdfDeviceCreate failed 0x%x\n", status));
        return status;
    }

	devExt = MySerialGetDeviceExtension(device);
	devExt->hasIncrementedIoSerialCount = FALSE;
	devExt->readBufferLength = 0;
	devExt->mustExit = FALSE;

	/*++
	Initialisation de la queue par defaut, qui va recevoir toutes les
	requetes non g�rees explicitement ailleurs.
	--*/
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead   = EvtIoRead;
    queueConfig.EvtIoWrite  = EvtIoWrite;
	queueConfig.EvtIoDeviceControl = EvtIoIoctl;
	queueConfig.EvtIoDefault = EvtIoDefault;


	status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &defaultqueue);
    if (!NT_SUCCESS(status))
	{
        KdPrint((DRIVER_NAME "WdfIoQueueCreate failed 0x%xn", status));
        return status;
    }


	/*++
	Cr�e un lien symbolique vers le device.
	TODO: d�terminer dynamiquement via le registre
	--*/
	status = RtlUnicodeStringPrintf(&symbolicLinkName,
                                    L"%ws%ws",
                                    L"\\DosDevices\\",
                                    L"COM1");
	if (!NT_SUCCESS(status))
	{
		KdPrint((DRIVER_NAME "Impossible de formater la string symbolicLinkName [0x%x]\n", status));
		return status;
	}
	status = WdfDeviceCreateSymbolicLink(device, &symbolicLinkName);
    if (!NT_SUCCESS(status))
	{
      KdPrint(("Impossible de creer le lien symbolique.\n"));
	  return status;
    }

	/*++
	Renseigner le lien symbolique dans la base de registre
	--*/
	status = RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP, L"SERIALCOMM",
                                   deviceName.Buffer,
                                   REG_SZ,
                                   L"COM1",
                                   deviceName.Length);
	if (!NT_SUCCESS(status))
	{
      KdPrint(("Impossible de renseigner la cl� de registre concernant le port COM.\n"));
	  return status;
    }

	/*++
	Notre device est maintenant cr�e,
	on incremente le compteur de port s�rie sur le syst�me.
	On d�crementera la valeur lors de la destruction du device ( appel � EvtDeviceContextCleanup )
	--*/
    (*countSoFar)++;
	devExt->hasIncrementedIoSerialCount = TRUE;

	WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &devExt->lock);
	InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
	status = PsCreateSystemThread(&devExt->readThread, 0L , &ObjectAttributes, NULL, NULL,
		ReadThread, devExt);

	KdPrint((DRIVER_NAME "<-- EvtDeviceAdd status=0x%x\n", status));
	return status;
}

VOID
EvtDeviceContextCleanup(
    WDFDEVICE       Device
    )
{
	PMY_SERIAL_DEVICE_EXTENSION devExt;
	PULONG	countSoFar;

	devExt = MySerialGetDeviceExtension(Device);
	if (devExt->hasIncrementedIoSerialCount)
	{
		countSoFar = &IoGetConfigurationInformation()->SerialCount;
		(*countSoFar)--;
	}
	devExt->mustExit = TRUE;
}
