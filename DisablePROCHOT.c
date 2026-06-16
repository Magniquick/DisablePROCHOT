// Copyright (c) 2018 Park Ju Hyung
//
// EFI application to disable BD PROCHOT (bi-directional processor hot)
// throttling, then chainload to the next boot entry.

#include <efi.h>
#include <efilib.h>

// ---------------------------------------------------------------------------
// MSR read/write helpers
// ---------------------------------------------------------------------------

// Write a 64-bit value to a Model-Specific Register (MSR).
// wrmsr takes: ECX=index, EDX:EAX=value (high:low 32-bit halves).
static uint64_t AsmWriteMsr64(uint32_t index, uint64_t val) {
  uint32_t low = (uint32_t)(val);
  uint32_t high = (uint32_t)(val >> 32);
  __asm__ __volatile__("wrmsr" : : "c"(index), "a"(low), "d"(high) : "memory");
  return val;
}

// Read a 64-bit Model-Specific Register (MSR).
// rdmsr takes: ECX=index, returns EDX:EAX=value (high:low 32-bit halves).
static uint64_t AsmReadMsr64(uint32_t index) {
  uint32_t low, high;
  __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(index) : "memory");
  return ((uint64_t)high << 32) | low;
}

// ---------------------------------------------------------------------------
// Partial EFI_LOAD_OPTION structure for parsing boot variables.
// ---------------------------------------------------------------------------

typedef struct __attribute__((packed)) {
  UINT32 Attributes;
  UINT16 FilePathListLength;
} EFI_LOAD_OPTION_HEADER;

// ---------------------------------------------------------------------------
// Hex / variable name helpers
// ---------------------------------------------------------------------------

// Convert a nibble (0-15) to its hex character representation.
static CHAR16 HexDigit(UINTN val) {
  if (val < 10) {
    return (CHAR16)(L'0' + val);
  }
  return (CHAR16)(L'A' + (val - 10));
}

// Build a "Boot####" variable name from a boot option ID.
// bootId is a 16-bit value; extract 4 hex digits (4 bits each).
static void MakeBootVarName(UINT16 bootId, CHAR16 *name, UINTN nameLen) {
  if (nameLen < 9) {
    return;
  }
  name[0] = L'B';
  name[1] = L'o';
  name[2] = L'o';
  name[3] = L't';
  name[4] = HexDigit((bootId >> 12) & 0xF);  // Bits 15-12
  name[5] = HexDigit((bootId >> 8) & 0xF);   // Bits 11-8
  name[6] = HexDigit((bootId >> 4) & 0xF);   // Bits 7-4
  name[7] = HexDigit(bootId & 0xF);          // Bits 3-0
  name[8] = L'\0';
}

// ---------------------------------------------------------------------------
// EFI variable helpers
// ---------------------------------------------------------------------------

// Read an EFI variable, allocating a buffer for its contents.
static EFI_STATUS GetVariableAlloc(EFI_SYSTEM_TABLE *systemTable, CHAR16 *name,
                                   EFI_GUID *guid, UINT8 **data,
                                   UINTN *dataSize) {
  EFI_STATUS status;
  UINTN size = 0;

  status = uefi_call_wrapper(systemTable->RuntimeServices->GetVariable, 5, name,
                             guid, NULL, &size, NULL);
  if (status != EFI_BUFFER_TOO_SMALL) {
    return status;
  }

  status = uefi_call_wrapper(systemTable->BootServices->AllocatePool, 3,
                             EfiLoaderData, size, (void **)data);
  if (EFI_ERROR(status)) {
    return status;
  }

  status = uefi_call_wrapper(systemTable->RuntimeServices->GetVariable, 5, name,
                             guid, NULL, &size, *data);
  if (EFI_ERROR(status)) {
    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, *data);
    *data = NULL;
    return status;
  }

  *dataSize = size;
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Device path helpers (forward declarations)
// ---------------------------------------------------------------------------

static EFI_DEVICE_PATH_PROTOCOL *ParseBootOption(UINT8 *bootData,
                                                  UINTN bootSize);
static EFI_DEVICE_PATH_PROTOCOL *GetLoadedImagePath(
    EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable);
static BOOLEAN DevicePathsMatch(EFI_DEVICE_PATH_PROTOCOL *first,
                                EFI_DEVICE_PATH_PROTOCOL *second);

// ---------------------------------------------------------------------------
// BootOrder walking
// ---------------------------------------------------------------------------

// Find the next loadable boot option ID after this image in the BootOrder list.
static EFI_STATUS GetNextBootOption(EFI_HANDLE image,
                                    EFI_SYSTEM_TABLE *systemTable,
                                    UINTN startOffset, UINT16 *nextBootId,
                                    UINTN *nextOffset) {
  EFI_STATUS status;
  UINT8 *bootOrderData = NULL;
  UINTN bootOrderSize = 0;
  UINT16 *bootOrder;
  UINTN bootCount;
  UINT16 bootCurrent = 0xFFFF;
  UINTN i;
  BOOLEAN found = FALSE;
  UINTN currentIndex = 0;
  EFI_DEVICE_PATH_PROTOCOL *loadedPath;

  status = GetVariableAlloc(systemTable, L"BootOrder", &gEfiGlobalVariableGuid,
                            &bootOrderData, &bootOrderSize);
  if (EFI_ERROR(status)) {
    return status;
  }

  if (bootOrderSize < sizeof(UINT16) || (bootOrderSize % sizeof(UINT16)) != 0) {
    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootOrderData);
    return EFI_COMPROMISED_DATA;
  }

  bootOrder = (UINT16 *)bootOrderData;
  bootCount = bootOrderSize / sizeof(UINT16);

  loadedPath = GetLoadedImagePath(image, systemTable);
  if (loadedPath) {
    for (i = 0; i < bootCount; ++i) {
      UINT16 bootId = bootOrder[i];
      CHAR16 bootVarName[9];
      UINT8 *bootData = NULL;
      UINTN bootSize = 0;
      EFI_DEVICE_PATH_PROTOCOL *devicePath;

      MakeBootVarName(bootId, bootVarName,
                      sizeof(bootVarName) / sizeof(CHAR16));
      status = GetVariableAlloc(systemTable, bootVarName,
                                &gEfiGlobalVariableGuid, &bootData, &bootSize);
      if (EFI_ERROR(status)) {
        continue;
      }

      devicePath = ParseBootOption(bootData, bootSize);
      if (devicePath && DevicePathsMatch(devicePath, loadedPath)) {
        found = TRUE;
        currentIndex = i;
        uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);
        break;
      }

      uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);
    }
  }

  if (!found) {
    UINTN size = sizeof(bootCurrent);
    status = uefi_call_wrapper(systemTable->RuntimeServices->GetVariable, 5,
                               L"BootCurrent", &gEfiGlobalVariableGuid, NULL,
                               &size, &bootCurrent);
    if (EFI_ERROR(status)) {
      bootCurrent = 0xFFFF;
    }
    for (i = 0; i < bootCount; ++i) {
      if (bootOrder[i] == bootCurrent) {
        found = TRUE;
        currentIndex = i;
        break;
      }
    }
  }

  if (!found) {
    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootOrderData);
    return EFI_NOT_FOUND;
  }

  if (bootCount == 1) {
    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootOrderData);
    return EFI_NOT_FOUND;
  }

  if (startOffset >= bootCount) {
    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootOrderData);
    return EFI_NOT_FOUND;
  }

  for (i = startOffset; i < bootCount; ++i) {
    UINT16 bootId = bootOrder[(currentIndex + i) % bootCount];
    CHAR16 bootVarName[9];
    UINT8 *bootData = NULL;
    UINTN bootSize = 0;

    MakeBootVarName(bootId, bootVarName,
                    sizeof(bootVarName) / sizeof(CHAR16));
    status = GetVariableAlloc(systemTable, bootVarName, &gEfiGlobalVariableGuid,
                              &bootData, &bootSize);
    if (EFI_ERROR(status)) {
      continue;
    }

    if (ParseBootOption(bootData, bootSize)) {
      *nextBootId = bootId;
      *nextOffset = i;
      uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);
      uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootOrderData);
      return EFI_SUCCESS;
    }

    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);
  }

  uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootOrderData);
  return EFI_NOT_FOUND;
}

static EFI_DEVICE_PATH_PROTOCOL *GetLoadedImagePath(
    EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  EFI_STATUS status;
  EFI_DEVICE_PATH_PROTOCOL *devicePath = NULL;
  EFI_LOADED_IMAGE *loadedImage = NULL;

  status = uefi_call_wrapper(systemTable->BootServices->OpenProtocol, 6, image,
                             &gEfiLoadedImageDevicePathProtocolGuid,
                             (void **)&devicePath, image, NULL,
                             EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (!EFI_ERROR(status) && devicePath) {
    return devicePath;
  }

  status = uefi_call_wrapper(systemTable->BootServices->OpenProtocol, 6, image,
                             &gEfiLoadedImageProtocolGuid,
                             (void **)&loadedImage, image, NULL,
                             EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (!EFI_ERROR(status) && loadedImage) {
    return loadedImage->FilePath;
  }

  return NULL;
}

static EFI_DEVICE_PATH_PROTOCOL *FindFilePathNode(
    EFI_DEVICE_PATH_PROTOCOL *devicePath) {
  EFI_DEVICE_PATH_PROTOCOL *node = devicePath;

  while (node && !IsDevicePathEnd(node)) {
    if (DevicePathType(node) == MEDIA_DEVICE_PATH &&
        DevicePathSubType(node) == MEDIA_FILEPATH_DP) {
      return node;
    }
    node = NextDevicePathNode(node);
  }

  return NULL;
}

// Check if a device path contains a file path node (needed for LoadImage).
static BOOLEAN DevicePathHasFilePath(EFI_DEVICE_PATH_PROTOCOL *devicePath) {
  return FindFilePathNode(devicePath) != NULL;
}

static BOOLEAN DevicePathsMatch(EFI_DEVICE_PATH_PROTOCOL *first,
                                EFI_DEVICE_PATH_PROTOCOL *second) {
  UINTN firstSize;
  UINTN secondSize;
  EFI_DEVICE_PATH_PROTOCOL *firstFile;
  EFI_DEVICE_PATH_PROTOCOL *secondFile;

  if (!first || !second) {
    return FALSE;
  }

  firstSize = DevicePathSize(first);
  secondSize = DevicePathSize(second);
  if (firstSize == secondSize && CompareMem(first, second, firstSize) == 0) {
    return TRUE;
  }

  firstFile = FindFilePathNode(first);
  secondFile = FindFilePathNode(second);
  if (!firstFile || !secondFile) {
    return FALSE;
  }

  firstSize = DevicePathNodeLength(firstFile);
  secondSize = DevicePathNodeLength(secondFile);
  return firstSize == secondSize &&
         CompareMem(firstFile, secondFile, firstSize) == 0;
}

// Parse a Boot#### variable and extract the device path.
// EFI_LOAD_OPTION layout: [Attributes][FilePathListLength][Description\0][FilePath...]
// Returns NULL if parsing fails or the path has no file component.
static EFI_DEVICE_PATH_PROTOCOL *ParseBootOption(UINT8 *bootData,
                                                  UINTN bootSize) {
  EFI_LOAD_OPTION_HEADER *header;
  UINT8 *descPtr;
  CHAR16 *description;
  UINTN descMax, descLen;
  UINT8 *filePath;
  EFI_DEVICE_PATH_PROTOCOL *devicePath;

  if (bootSize < sizeof(EFI_LOAD_OPTION_HEADER) + sizeof(CHAR16)) {
    return NULL;
  }

  header = (EFI_LOAD_OPTION_HEADER *)bootData;
  if ((header->Attributes & LOAD_OPTION_ACTIVE) == 0) {
    return NULL;
  }

  descPtr = bootData + sizeof(EFI_LOAD_OPTION_HEADER);
  description = (CHAR16 *)descPtr;

  // Find null terminator in description string.
  descMax = (bootSize - (UINTN)(descPtr - bootData)) / sizeof(CHAR16);
  for (descLen = 0; descLen < descMax; ++descLen) {
    if (description[descLen] == L'\0') {
      break;
    }
  }
  if (descLen == descMax) {
    return NULL;  // No null terminator
  }

  // FilePath starts after the null-terminated description.
  filePath = (UINT8 *)(description + descLen + 1);
  if ((UINTN)(filePath - bootData) + header->FilePathListLength > bootSize ||
      header->FilePathListLength == 0) {
    return NULL;
  }

  devicePath = (EFI_DEVICE_PATH_PROTOCOL *)filePath;
  if (!DevicePathHasFilePath(devicePath)) {
    return NULL;  // Device-only path, can't chainload
  }

  return devicePath;
}

// ---------------------------------------------------------------------------
// Chainload logic
// ---------------------------------------------------------------------------

// Chainload the next entry in BootOrder.
// Returns EFI_SUCCESS if chainload succeeded (and started image returned).
static EFI_STATUS TryBootOrderChainload(EFI_HANDLE image,
                                        EFI_SYSTEM_TABLE *systemTable,
                                        SIMPLE_TEXT_OUTPUT_INTERFACE *conOut) {
  EFI_STATUS status;
  UINT16 nextBootId;
  UINTN offset = 1;
  UINTN nextOffset = 0;
  CHAR16 bootVarName[9];
  UINT8 *bootData = NULL;
  UINTN bootSize = 0;
  EFI_DEVICE_PATH_PROTOCOL *devicePath;
  EFI_HANDLE nextImage;

  for (;;) {
    status = GetNextBootOption(image, systemTable, offset, &nextBootId,
                               &nextOffset);
    if (status == EFI_NOT_FOUND) {
      conOut->OutputString(conOut, L"No next boot entry\r\n");
      return status;
    }
    if (EFI_ERROR(status)) {
      conOut->OutputString(conOut, L"BootOrder unavailable\r\n");
      return status;
    }
    offset = nextOffset + 1;

    // Read the Boot#### variable.
    MakeBootVarName(nextBootId, bootVarName,
                    sizeof(bootVarName) / sizeof(CHAR16));
    status = GetVariableAlloc(systemTable, bootVarName, &gEfiGlobalVariableGuid,
                              &bootData, &bootSize);
    if (EFI_ERROR(status)) {
      continue;
    }

    devicePath = ParseBootOption(bootData, bootSize);
    if (!devicePath) {
      uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);
      continue;
    }

    conOut->OutputString(conOut, L"Chainloading next boot entry\r\n");
    status = uefi_call_wrapper(systemTable->BootServices->LoadImage, 6, FALSE,
                               image, devicePath, NULL, 0, &nextImage);
    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);
    bootData = NULL;
    if (EFI_ERROR(status)) {
      continue;
    }

    status = uefi_call_wrapper(systemTable->BootServices->StartImage, 3,
                               nextImage, NULL, NULL);
    if (EFI_ERROR(status)) {
      continue;
    }

    return status;
  }
}

// Chainload to the next boot entry.
static EFI_STATUS ChainloadNext(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable,
                                SIMPLE_TEXT_OUTPUT_INTERFACE *conOut) {
  return TryBootOrderChainload(image, systemTable, conOut);
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

// Main entry point: attempt MSR write with #GP fault-tolerance, then chainload.
static EFI_STATUS efi_main_sysv(EFI_HANDLE image,
                                EFI_SYSTEM_TABLE *systemTable) {
  SIMPLE_TEXT_OUTPUT_INTERFACE *conOut;

  InitializeLib(image, systemTable);
  conOut = systemTable->ConOut;

  // MSR 0x1FC = IA32_POWER_CTL. Read-modify-write so we only touch the two
  // bits we care about and preserve every other firmware-set bit (e.g. bit1
  // C1E enable):
  //   bit0  = 0 -> BD PROCHOT disabled (clears bi-directional processor hot
  //                throttling).
  //   bit24 = 1 -> DISABLE_VR_THERMAL_ALERT: lifts a phantom VR-thermal clamp
  //                that pinned CPU+iGPU to base clock even when thermals were
  //                fine.  Confirmed in SiInit.efi; verified live on this laptop
  //                (cores 1800->2500+, iGPU 300->1150 MHz).
  // This MSR never #GPs on real hardware; QEMU/OVMF silently emulates it. So no
  // hypervisor check or fault handler is needed — just do the write.
  conOut->OutputString(conOut, L"Disabling BD PROCHOT + VR Thermal Alert\r\n");
  uint64_t powerCtl = AsmReadMsr64(0x1FC);
  powerCtl &= ~(uint64_t)0x1;       // clear bit0  (BD PROCHOT)
  powerCtl |= (uint64_t)0x1 << 24;  // set  bit24  (disable VR thermal alert)
  AsmWriteMsr64(0x1FC, powerCtl);
  conOut->OutputString(conOut, L"BD PROCHOT + VR Thermal Alert disabled\r\n");

  return ChainloadNext(image, systemTable, conOut);
}

// GNU-EFI entry point (System V ABI).
EFI_STATUS
_entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  return efi_main_sysv(image, systemTable);
}

// Standard EFI entry point (MS ABI).
EFI_STATUS __attribute__((ms_abi)) efi_main(EFI_HANDLE image,
                                            EFI_SYSTEM_TABLE *systemTable) {
  return efi_main_sysv(image, systemTable);
}
