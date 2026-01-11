// Copyright (c) 2018 Park Ju Hyung
//
// EFI application to disable BD PROCHOT (bi-directional processor hot)
// throttling, then chainload to the next boot entry.

#include <efi.h>
#include <efilib.h>

// Write a 64-bit value to a Model-Specific Register (MSR).
// wrmsr takes: ECX=index, EDX:EAX=value (high:low 32-bit halves).
static uint64_t AsmWriteMsr64(uint32_t index, uint64_t val) {
  uint32_t low = (uint32_t)(val);
  uint32_t high = (uint32_t)(val >> 32);
  __asm__ __volatile__("wrmsr" : : "c"(index), "a"(low), "d"(high));
  return val;
}

// Check CPUID leaf 1 ECX bit 31 to detect if running under a hypervisor.
// Bit 31 is set by VMMs to indicate guest mode (Intel/AMD spec).
static int IsHypervisorPresent(void) {
  uint32_t eax_out, ebx_out, ecx, edx_out;
  __asm__ __volatile__("cpuid"
                       : "=a"(eax_out), "=b"(ebx_out), "=c"(ecx), "=d"(edx_out)
                       : "0"(1));
  (void)eax_out;
  (void)ebx_out;
  (void)edx_out;
  return (ecx >> 31) & 1;  // Extract bit 31
}

// Partial EFI_LOAD_OPTION structure for parsing boot variables.
typedef struct __attribute__((packed)) {
  UINT32 Attributes;
  UINT16 FilePathListLength;
} EFI_LOAD_OPTION_HEADER;

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

// Find the next boot option ID after BootCurrent in the BootOrder list.
static EFI_STATUS GetNextBootOption(EFI_SYSTEM_TABLE *systemTable,
                                    UINT16 *nextBootId) {
  EFI_STATUS status;
  UINT8 *bootOrderData = NULL;
  UINTN bootOrderSize = 0;
  UINT16 *bootOrder;
  UINTN bootCount;
  UINT16 bootCurrent = 0xFFFF;
  UINTN i;
  BOOLEAN found = FALSE;
  UINTN currentIndex = 0;

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

  {
    UINTN size = sizeof(bootCurrent);
    status = uefi_call_wrapper(systemTable->RuntimeServices->GetVariable, 5,
                               L"BootCurrent", &gEfiGlobalVariableGuid, NULL,
                               &size, &bootCurrent);
    if (EFI_ERROR(status)) {
      bootCurrent = 0xFFFF;
    }
  }

  for (i = 0; i < bootCount; ++i) {
    if (bootOrder[i] == bootCurrent) {
      found = TRUE;
      currentIndex = i;
      break;
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

  *nextBootId = bootOrder[(currentIndex + 1) % bootCount];

  uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootOrderData);
  return EFI_SUCCESS;
}

// Check if a device path contains a file path node (needed for LoadImage).
static BOOLEAN DevicePathHasFilePath(EFI_DEVICE_PATH_PROTOCOL *devicePath) {
  EFI_DEVICE_PATH_PROTOCOL *node = devicePath;

  while (!IsDevicePathEnd(node)) {
    if (DevicePathType(node) == MEDIA_DEVICE_PATH &&
        DevicePathSubType(node) == MEDIA_FILEPATH_DP) {
      return TRUE;
    }
    node = NextDevicePathNode(node);
  }

  return FALSE;
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

// Chainload the next entry in BootOrder.
// Returns EFI_SUCCESS if chainload succeeded (and started image returned).
static EFI_STATUS TryBootOrderChainload(EFI_HANDLE image,
                                        EFI_SYSTEM_TABLE *systemTable,
                                        SIMPLE_TEXT_OUTPUT_INTERFACE *conOut) {
  EFI_STATUS status;
  UINT16 nextBootId;
  CHAR16 bootVarName[9];
  UINT8 *bootData = NULL;
  UINTN bootSize = 0;
  EFI_DEVICE_PATH_PROTOCOL *devicePath;
  EFI_HANDLE nextImage;

  status = GetNextBootOption(systemTable, &nextBootId);
  if (status == EFI_NOT_FOUND) {
    conOut->OutputString(conOut, L"No next boot entry\r\n");
    return status;
  }
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"BootOrder unavailable\r\n");
    return status;
  }

  // Read the Boot#### variable.
  MakeBootVarName(nextBootId, bootVarName, sizeof(bootVarName) / sizeof(CHAR16));
  status = GetVariableAlloc(systemTable, bootVarName, &gEfiGlobalVariableGuid,
                            &bootData, &bootSize);
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"Boot option missing\r\n");
    return status;
  }

  devicePath = ParseBootOption(bootData, bootSize);
  if (!devicePath) {
    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);
    return EFI_COMPROMISED_DATA;
  }

  conOut->OutputString(conOut, L"Chainloading next boot entry\r\n");
  status = uefi_call_wrapper(systemTable->BootServices->LoadImage, 6, FALSE,
                             image, devicePath, NULL, 0, &nextImage);
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"LoadImage failed\r\n");
    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);
    return status;
  }

  uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);

  status = uefi_call_wrapper(systemTable->BootServices->StartImage, 3,
                             nextImage, NULL, NULL);
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"StartImage failed\r\n");
  }
  return status;
}

// Chainload to the next boot entry.
static EFI_STATUS ChainloadNext(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable,
                                SIMPLE_TEXT_OUTPUT_INTERFACE *conOut) {
  return TryBootOrderChainload(image, systemTable, conOut);
}

// Main entry point: disable BD PROCHOT if on real hardware, then chainload.
static EFI_STATUS efi_main_sysv(EFI_HANDLE image,
                                EFI_SYSTEM_TABLE *systemTable) {
  SIMPLE_TEXT_OUTPUT_INTERFACE *conOut;

  InitializeLib(image, systemTable);
  conOut = systemTable->ConOut;

  if (IsHypervisorPresent()) {
    conOut->OutputString(conOut,
                         L"Hypervisor detected, skipping MSR write\r\n");
  } else {
    conOut->OutputString(conOut, L"Disabling BD PROCHOT\r\n");
    // MSR 0x1FC = IA32_POWER_CTL. Writing 0 clears bit 0 (BD PROCHOT enable),
    // disabling bi-directional processor hot throttling.
    AsmWriteMsr64(0x1FC, 0);
    conOut->OutputString(conOut, L"BD PROCHOT disabled\r\n");
  }

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
