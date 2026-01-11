// Test helper: Set up BootOrder with DisablePROCHOT -> ChainSuccess,
// then launch DisablePROCHOT to test the BootOrder chainload path.

#include <efi.h>
#include <efilib.h>

// EFI_LOAD_OPTION structure for boot variables.
// Layout: [Attributes][FilePathListLength][Description\0][FilePath...]
static EFI_STATUS CreateBootOption(EFI_SYSTEM_TABLE *systemTable,
                                   UINT16 bootId,
                                   CHAR16 *description,
                                   CHAR16 *filePath,
                                   EFI_HANDLE deviceHandle) {
  EFI_STATUS status;
  EFI_DEVICE_PATH_PROTOCOL *devicePath;
  UINTN devicePathSize;
  UINTN descSize;
  UINTN totalSize;
  UINT8 *buffer;
  UINT8 *ptr;
  CHAR16 varName[9];

  // Build device path for the file.
  devicePath = FileDevicePath(deviceHandle, filePath);
  if (!devicePath) {
    return EFI_OUT_OF_RESOURCES;
  }
  devicePathSize = DevicePathSize(devicePath);

  // Calculate sizes.
  descSize = (StrLen(description) + 1) * sizeof(CHAR16);
  totalSize = sizeof(UINT32) + sizeof(UINT16) + descSize + devicePathSize;

  // Allocate buffer.
  status = uefi_call_wrapper(systemTable->BootServices->AllocatePool, 3,
                             EfiLoaderData, totalSize, (void **)&buffer);
  if (EFI_ERROR(status)) {
    return status;
  }

  // Build EFI_LOAD_OPTION.
  ptr = buffer;

  // Attributes (LOAD_OPTION_ACTIVE = 0x00000001)
  *(UINT32 *)ptr = 0x00000001;
  ptr += sizeof(UINT32);

  // FilePathListLength
  *(UINT16 *)ptr = (UINT16)devicePathSize;
  ptr += sizeof(UINT16);

  // Description (null-terminated UTF-16)
  CopyMem(ptr, description, descSize);
  ptr += descSize;

  // FilePath (device path)
  CopyMem(ptr, devicePath, devicePathSize);

  // Build variable name "Boot####"
  varName[0] = L'B';
  varName[1] = L'o';
  varName[2] = L'o';
  varName[3] = L't';
  varName[4] = (bootId >> 12) < 10 ? L'0' + (bootId >> 12) : L'A' + (bootId >> 12) - 10;
  varName[5] = ((bootId >> 8) & 0xF) < 10 ? L'0' + ((bootId >> 8) & 0xF) : L'A' + ((bootId >> 8) & 0xF) - 10;
  varName[6] = ((bootId >> 4) & 0xF) < 10 ? L'0' + ((bootId >> 4) & 0xF) : L'A' + ((bootId >> 4) & 0xF) - 10;
  varName[7] = (bootId & 0xF) < 10 ? L'0' + (bootId & 0xF) : L'A' + (bootId & 0xF) - 10;
  varName[8] = L'\0';

  // Write the boot variable.
  status = uefi_call_wrapper(systemTable->RuntimeServices->SetVariable, 5,
                             varName, &gEfiGlobalVariableGuid,
                             EFI_VARIABLE_NON_VOLATILE |
                             EFI_VARIABLE_BOOTSERVICE_ACCESS |
                             EFI_VARIABLE_RUNTIME_ACCESS,
                             totalSize, buffer);

  uefi_call_wrapper(systemTable->BootServices->FreePool, 1, buffer);
  return status;
}

static EFI_STATUS efi_main_impl(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE *loadedImage = NULL;
  EFI_HANDLE deviceHandle;
  SIMPLE_TEXT_OUTPUT_INTERFACE *conOut = systemTable->ConOut;
  // Boot0002 is what firmware sets BootCurrent to when booting BOOTX64.EFI.
  // We overwrite Boot0002 to point to DisablePROCHOT, and Boot0003 to ChainSuccess.
  UINT16 bootOrder[2] = {0x0002, 0x0003};
  EFI_DEVICE_PATH_PROTOCOL *disablePath;
  EFI_HANDLE disableImage;

  InitializeLib(image, systemTable);

  conOut->OutputString(conOut, L"SetBootOrder: Setting up boot variables\r\n");

  // Get our device handle.
  status = uefi_call_wrapper(systemTable->BootServices->OpenProtocol, 6, image,
                             &gEfiLoadedImageProtocolGuid, (void **)&loadedImage,
                             image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"Failed to get LoadedImage\r\n");
    return status;
  }
  deviceHandle = loadedImage->DeviceHandle;

  // Create Boot0002 -> DisablePROCHOT.efi (overwrites firmware's entry)
  status = CreateBootOption(systemTable, 0x0002, L"DisablePROCHOT",
                            L"\\EFI\\BOOT\\DisablePROCHOT.efi", deviceHandle);
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"Failed to create Boot0002\r\n");
    return status;
  }
  conOut->OutputString(conOut, L"Created Boot0002 -> DisablePROCHOT.efi\r\n");

  // Create Boot0003 -> ChainSuccess.efi
  status = CreateBootOption(systemTable, 0x0003, L"ChainSuccess",
                            L"\\EFI\\BOOT\\ChainSuccess.efi", deviceHandle);
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"Failed to create Boot0003\r\n");
    return status;
  }
  conOut->OutputString(conOut, L"Created Boot0003 -> ChainSuccess.efi\r\n");

  // Set BootOrder = {0002, 0003}
  status = uefi_call_wrapper(systemTable->RuntimeServices->SetVariable, 5,
                             L"BootOrder", &gEfiGlobalVariableGuid,
                             EFI_VARIABLE_NON_VOLATILE |
                             EFI_VARIABLE_BOOTSERVICE_ACCESS |
                             EFI_VARIABLE_RUNTIME_ACCESS,
                             sizeof(bootOrder), bootOrder);
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"Failed to set BootOrder\r\n");
    return status;
  }
  conOut->OutputString(conOut, L"Set BootOrder = {0002, 0003}\r\n");

  // BootCurrent should already be 0002 (set by firmware when it booted us via BOOTX64.EFI)
  conOut->OutputString(conOut, L"BootCurrent = 0002 (set by firmware)\r\n");

  // Now load and start DisablePROCHOT.efi
  conOut->OutputString(conOut, L"Launching DisablePROCHOT.efi...\r\n");

  disablePath = FileDevicePath(deviceHandle, L"\\EFI\\BOOT\\DisablePROCHOT.efi");
  if (!disablePath) {
    conOut->OutputString(conOut, L"Failed to create device path\r\n");
    return EFI_OUT_OF_RESOURCES;
  }

  status = uefi_call_wrapper(systemTable->BootServices->LoadImage, 6,
                             FALSE, image, disablePath, NULL, 0, &disableImage);
  if (EFI_ERROR(status)) {
    conOut->OutputString(conOut, L"Failed to load DisablePROCHOT.efi\r\n");
    return status;
  }

  status = uefi_call_wrapper(systemTable->BootServices->StartImage, 3,
                             disableImage, NULL, NULL);
  return status;
}

EFI_STATUS _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  return efi_main_impl(image, systemTable);
}

EFI_STATUS __attribute__((ms_abi)) efi_main(EFI_HANDLE image,
                                            EFI_SYSTEM_TABLE *systemTable) {
  return efi_main_impl(image, systemTable);
}
