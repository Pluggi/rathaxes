/*++
driver.c
Le point d'entr�e du driver
--*/

#include "driver.h"

/*++
Infos sur les #pragma alloc_text(section, fonction1, ...) :
- Indique la section de code dans laquelle doit resider le code de la / des fonction(s),
par defaut le code reside dans le section .text du binaire
- Fonctionne avec le linkage C (donc, pour du C++, utiliser extern "C")
- La directive doit apparaitre apr�s la d�claration de la fonction, mais avant sa d�finition
- Les fonctions indiqu�es doivent etre dans la meme unit� de compilation que le pragma,
sinon, le programme fonctionnera (surement) mais le code des fonctions ne sera pas dans les sections voulues.
- Le pragma alloc_text ne peut pas etre utilis� dans une fonction
--*/
#ifdef ALLOC_PRAGMA
/*++
La section INIT indique que le code ne sera utilis� qu'une seule fois.
La m�moire utilis�e par le code de la fonction pourra etre lib�r� apr�s la fin du premier appel.
Ici, le point d'entr�e du driver est donc plac� dans la section INIT.
--*/
# pragma alloc_text(INIT, DriverEntry)
#endif

/*++
Point d'entr�e du driver
--*/
NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG	config;
    NTSTATUS			status;

	KdPrint((DRIVER_NAME "-->DriverEntry\n"));
	KdPrint((DRIVER_NAME " - Very simple RS232 driver [Rathaxes Experiments]\n"));
	KdPrint((DRIVER_NAME " - Built %s %s\n", __DATE__, __TIME__));
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    status = WdfDriverCreate(DriverObject,
                            RegistryPath,
                            WDF_NO_OBJECT_ATTRIBUTES,
                            &config,
                            WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
	{
        KdPrint((DRIVER_NAME " - Error: WdfDriverCreate failed 0x%x\n", status));
	}
	KdPrint((DRIVER_NAME "<--DriverEntry\n"));
	return status;
}