// Copyright (c) 2018 Park Ju Hyung

#include <efi.h>
#include <efilib.h>

static uint64_t AsmWriteMsr64(uint32_t index, uint64_t val) {
  uint32_t low;
  uint32_t high;

  low = (uint32_t)(val);
  high = (uint32_t)(val >> 32);

  __asm__ __volatile__("wrmsr" : : "c"(index), "a"(low), "d"(high));

  return val;
}

static int IsHypervisorPresent(void) {
  uint32_t eax_out, ebx_out, ecx, edx_out;

  /* CPUID leaf 1, ECX bit 31 indicates hypervisor presence */
  __asm__ __volatile__("cpuid"
                       : "=a"(eax_out), "=b"(ebx_out), "=c"(ecx), "=d"(edx_out)
                       : "0"(1));

  (void)eax_out;
  (void)ebx_out;
  (void)edx_out;
  return (ecx >> 31) & 1;
}

typedef struct __attribute__((packed)) {
  UINT32 Attributes;
  UINT16 FilePathListLength;
} EFI_LOAD_OPTION_HEADER;

static CHAR16 HexDigit(UINTN val) {
  if (val < 10) {
    return (CHAR16)(L'0' + val);
  }
  return (CHAR16)(L'A' + (val - 10));
}

static void PrintStatus(SIMPLE_TEXT_OUTPUT_INTERFACE *conOut,
                        EFI_STATUS status) {
  CHAR16 buf[3 + (sizeof(EFI_STATUS) * 2) + 2];
  UINTN i;
  buf[0] = L' ';
  buf[1] = L'0';
  buf[2] = L'x';
  for (i = 0; i < sizeof(EFI_STATUS) * 2; ++i) {
    UINTN shift = (sizeof(EFI_STATUS) * 8 - 4) - (i * 4);
    UINTN nibble = (status >> shift) & 0xF;
    buf[3 + i] = HexDigit(nibble);
  }
  buf[3 + i] = L'\r';
  buf[4 + i] = L'\n';
  buf[5 + i] = L'\0';
  conOut->OutputString(conOut, buf);
}

static void MakeBootVarName(UINT16 bootId, CHAR16 *name, UINTN nameLen) {
  if (nameLen < 9) {
    return;
  }

  name[0] = L'B';
  name[1] = L'o';
  name[2] = L'o';
  name[3] = L't';
  name[4] = HexDigit((bootId >> 12) & 0xF);
  name[5] = HexDigit((bootId >> 8) & 0xF);
  name[6] = HexDigit((bootId >> 4) & 0xF);
  name[7] = HexDigit(bootId & 0xF);
  name[8] = L'\0';
}

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

static EFI_STATUS ChainloadNext(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable,
                                SIMPLE_TEXT_OUTPUT_INTERFACE *conOut) {
  EFI_STATUS status;
  UINT16 nextBootId;
  CHAR16 bootVarName[9];
  UINT8 *bootData = NULL;
  UINTN bootSize = 0;
  EFI_LOAD_OPTION_HEADER *header;
  UINT8 *descPtr;
  CHAR16 *description;
  UINTN descMax;
  UINTN descLen = 0;
  UINT8 *filePath;
  EFI_DEVICE_PATH_PROTOCOL *devicePath;
  EFI_HANDLE nextImage;
  EFI_LOADED_IMAGE *loadedImage = NULL;
  EFI_DEVICE_PATH_PROTOCOL *fallbackPath = NULL;
  EFI_HANDLE *fsHandles = NULL;
  UINTN fsHandleCount = 0;
  EFI_STATUS imgStatus;

  imgStatus =
      uefi_call_wrapper(systemTable->BootServices->OpenProtocol, 6, image,
                        &gEfiLoadedImageProtocolGuid, (void **)&loadedImage,
                        image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

  status = GetNextBootOption(systemTable, &nextBootId);
  if (status == EFI_NOT_FOUND) {
    conOut->OutputString(conOut, L"No next boot entry\r\n");
  }
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"BootOrder unavailable\r\n");
  }

  if (!EFI_ERROR(status)) {
    MakeBootVarName(nextBootId, bootVarName,
                    sizeof(bootVarName) / sizeof(CHAR16));
    status = GetVariableAlloc(systemTable, bootVarName, &gEfiGlobalVariableGuid,
                              &bootData, &bootSize);
    if (!EFI_ERROR(status)) {
      if (bootSize < sizeof(EFI_LOAD_OPTION_HEADER) + sizeof(CHAR16)) {
        status = EFI_COMPROMISED_DATA;
      } else {
        header = (EFI_LOAD_OPTION_HEADER *)bootData;
        descPtr = bootData + sizeof(EFI_LOAD_OPTION_HEADER);
        description = (CHAR16 *)descPtr;
        descMax = (bootSize - (UINTN)(descPtr - bootData)) / sizeof(CHAR16);
        for (descLen = 0; descLen < descMax; ++descLen) {
          if (description[descLen] == L'\0') {
            break;
          }
        }
        if (descLen == descMax) {
          status = EFI_COMPROMISED_DATA;
        } else {
          filePath = (UINT8 *)(description + descLen + 1);
          if ((UINTN)(filePath - bootData) + header->FilePathListLength >
                  bootSize ||
              header->FilePathListLength == 0) {
            status = EFI_COMPROMISED_DATA;
          } else {
            devicePath = (EFI_DEVICE_PATH_PROTOCOL *)filePath;
            if (!DevicePathHasFilePath(devicePath)) {
              status = EFI_UNSUPPORTED;
            } else {
              conOut->OutputString(conOut, L"Chainloading next boot entry\r\n");
              status = uefi_call_wrapper(systemTable->BootServices->LoadImage,
                                         6, FALSE, image, devicePath, NULL, 0,
                                         &nextImage);
              if (!EFI_ERROR(status)) {
                status =
                    uefi_call_wrapper(systemTable->BootServices->StartImage, 3,
                                      nextImage, NULL, NULL);
                if (EFI_ERROR(status)) {
                  conOut->OutputString(conOut, L"StartImage failed\r\n");
                }
                uefi_call_wrapper(systemTable->BootServices->FreePool, 1,
                                  bootData);
                return status;
              } else {
                conOut->OutputString(conOut, L"LoadImage failed\r\n");
              }
            }
          }
        }
      }
    } else {
      conOut->OutputString(conOut, L"Boot option missing\r\n");
    }
  }

  if (bootData) {
    uefi_call_wrapper(systemTable->BootServices->FreePool, 1, bootData);
  }

  if (EFI_ERROR(imgStatus)) {
    conOut->OutputString(conOut, L"OpenProtocol(LoadedImage) failed\r\n");
    PrintStatus(conOut, imgStatus);
  }

  if (loadedImage) {
    fallbackPath =
        FileDevicePath(loadedImage->DeviceHandle, L"\\EFI\\BOOT\\CHAIN.EFI");
  } else {
    status = uefi_call_wrapper(systemTable->BootServices->LocateHandleBuffer, 5,
                               ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                               NULL, &fsHandleCount, &fsHandles);
    if (!EFI_ERROR(status) && fsHandleCount > 0) {
      fallbackPath = FileDevicePath(fsHandles[0], L"\\EFI\\BOOT\\CHAIN.EFI");
    } else {
      conOut->OutputString(conOut, L"LocateHandleBuffer(SimpleFS) failed\r\n");
      PrintStatus(conOut, status);
      // Try to connect all controllers then retry SimpleFS
      if (status == EFI_NOT_FOUND) {
        EFI_HANDLE *allHandles = NULL;
        UINTN allCount = 0;
        status =
            uefi_call_wrapper(systemTable->BootServices->LocateHandleBuffer, 5,
                              AllHandles, NULL, NULL, &allCount, &allHandles);
        if (!EFI_ERROR(status) && allCount > 0 && allHandles) {
          UINTN idx;
          for (idx = 0; idx < allCount; ++idx) {
            uefi_call_wrapper(systemTable->BootServices->ConnectController, 5,
                              allHandles[idx], NULL, NULL, TRUE);
          }
          uefi_call_wrapper(systemTable->BootServices->FreePool, 1, allHandles);
          status = uefi_call_wrapper(
              systemTable->BootServices->LocateHandleBuffer, 5, ByProtocol,
              &gEfiSimpleFileSystemProtocolGuid, NULL, &fsHandleCount,
              &fsHandles);
          if (!EFI_ERROR(status) && fsHandleCount > 0) {
            fallbackPath =
                FileDevicePath(fsHandles[0], L"\\EFI\\BOOT\\CHAIN.EFI");
          }
        }
      }
    }
    if (fsHandles) {
      uefi_call_wrapper(systemTable->BootServices->FreePool, 1, fsHandles);
    }
  }

  if (fallbackPath) {
    conOut->OutputString(conOut,
                         L"Fallback chain to \\EFI\\BOOT\\CHAIN.EFI\r\n");
    status = uefi_call_wrapper(systemTable->BootServices->LoadImage, 6, FALSE,
                               image, fallbackPath, NULL, 0, &nextImage);
    if (!EFI_ERROR(status)) {
      status = uefi_call_wrapper(systemTable->BootServices->StartImage, 3,
                                 nextImage, NULL, NULL);
      if (EFI_ERROR(status)) {
        conOut->OutputString(conOut, L"StartImage failed\r\n");
      }
      return status;
    } else {
      conOut->OutputString(conOut, L"Fallback LoadImage failed\r\n");
    }
  } else {
    conOut->OutputString(conOut, L"No fallback device path\r\n");
  }

  return status;
}

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
    AsmWriteMsr64(0x1FC, 0);
    conOut->OutputString(conOut, L"BD PROCHOT disabled\r\n");
  }

  return ChainloadNext(image, systemTable, conOut);
}

EFI_STATUS
_entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  return efi_main_sysv(image, systemTable);
}

EFI_STATUS __attribute__((ms_abi)) efi_main(EFI_HANDLE image,
                                            EFI_SYSTEM_TABLE *systemTable) {
  return efi_main_sysv(image, systemTable);
}
