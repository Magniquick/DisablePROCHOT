// Test helper: set up BootOrder with DisablePROCHOT -> ChainSuccess (plus a
// few decoy entries), then launch DisablePROCHOT to exercise the chainload.
//
// Self-contained: GNU-EFI headers for TYPES only, no GNU-EFI lib, so it builds
// straight to PE-COFF with clang/lld like the main app.

#include <efi.h>

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_RUNTIME_SERVICES *RT;
static EFI_HANDLE IM;

static EFI_GUID gEfiGlobalVariableGuid = EFI_GLOBAL_VARIABLE;
static EFI_GUID gEfiLoadedImageProtocolGuid = LOADED_IMAGE_PROTOCOL;
static EFI_GUID gEfiDevicePathProtocolGuid = {
    0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

static void Out(CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, s); }

static UINTN StrLen16(CHAR16 *s) {
  UINTN n = 0;
  while (s[n]) n++;
  return n;
}

static void CopyMem8(void *d, const void *s, UINTN n) {
  UINT8 *dd = d;
  const UINT8 *ss = s;
  for (UINTN i = 0; i < n; i++) dd[i] = ss[i];
}

static UINTN DevPathSize(EFI_DEVICE_PATH_PROTOCOL *dp) {
  EFI_DEVICE_PATH_PROTOCOL *n = dp;
  UINTN sz = 0;
  while (n) {
    UINTN l = (UINTN)DevicePathNodeLength(n);
    if (l < sizeof(EFI_DEVICE_PATH_PROTOCOL)) return 0;
    sz += l;
    if (IsDevicePathEnd(n)) return sz;
    n = NextDevicePathNode(n);
  }
  return 0;
}

// Build  <partition device path> + File(file) + End  for a handle's volume.
static EFI_DEVICE_PATH_PROTOCOL *FileDevicePath16(EFI_HANDLE dev, CHAR16 *file) {
  EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
  UINT8 *out;
  UINTN dlen, fbytes, fnode;

  if (EFI_ERROR(BS->HandleProtocol(dev, &gEfiDevicePathProtocolGuid, (void **)&dp)) || !dp)
    return NULL;
  dlen = DevPathSize(dp);
  if (dlen < 4) return NULL;
  dlen -= 4;  // strip End
  fbytes = (StrLen16(file) + 1) * sizeof(CHAR16);
  fnode = 4 + fbytes;  // File node header (4) + path string

  if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, dlen + fnode + 4, (void **)&out)) || !out)
    return NULL;
  CopyMem8(out, dp, dlen);
  out[dlen + 0] = 0x04;  // MEDIA_DEVICE_PATH
  out[dlen + 1] = 0x04;  // MEDIA_FILEPATH_DP
  out[dlen + 2] = (UINT8)(fnode & 0xff);
  out[dlen + 3] = (UINT8)(fnode >> 8);
  CopyMem8(out + dlen + 4, file, fbytes);
  out[dlen + fnode + 0] = 0x7f;  // End-of-device-path
  out[dlen + fnode + 1] = 0xff;
  out[dlen + fnode + 2] = 0x04;
  out[dlen + fnode + 3] = 0x00;
  return (EFI_DEVICE_PATH_PROTOCOL *)out;
}

static EFI_STATUS CreateBootOption(UINT16 bootId, CHAR16 *description,
                                   CHAR16 *filePath, EFI_HANDLE deviceHandle) {
  EFI_STATUS status;
  UINT8 endDevicePath[] = {0x7f, 0xff, 0x04, 0x00};
  EFI_DEVICE_PATH_PROTOCOL *devicePath;
  UINTN devicePathSize, descSize, totalSize;
  UINT8 *buffer, *ptr;
  CHAR16 varName[9];
  const CHAR16 hex[] = L"0123456789ABCDEF";

  if (filePath) {
    devicePath = FileDevicePath16(deviceHandle, filePath);
    if (!devicePath) return EFI_OUT_OF_RESOURCES;
    devicePathSize = DevPathSize(devicePath);
  } else {
    devicePath = (EFI_DEVICE_PATH_PROTOCOL *)endDevicePath;
    devicePathSize = sizeof(endDevicePath);
  }

  descSize = (StrLen16(description) + 1) * sizeof(CHAR16);
  totalSize = sizeof(UINT32) + sizeof(UINT16) + descSize + devicePathSize;

  if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, totalSize, (void **)&buffer)))
    return EFI_OUT_OF_RESOURCES;

  ptr = buffer;
  *(UINT32 *)ptr = bootId == 0x0005 ? 0 : 0x00000001;  // LOAD_OPTION_ACTIVE
  ptr += sizeof(UINT32);
  *(UINT16 *)ptr = (UINT16)devicePathSize;
  ptr += sizeof(UINT16);
  CopyMem8(ptr, description, descSize);
  ptr += descSize;
  CopyMem8(ptr, devicePath, devicePathSize);

  varName[0] = L'B';
  varName[1] = L'o';
  varName[2] = L'o';
  varName[3] = L't';
  varName[4] = hex[(bootId >> 12) & 0xF];
  varName[5] = hex[(bootId >> 8) & 0xF];
  varName[6] = hex[(bootId >> 4) & 0xF];
  varName[7] = hex[bootId & 0xF];
  varName[8] = L'\0';

  status = RT->SetVariable(varName, &gEfiGlobalVariableGuid,
                           EFI_VARIABLE_NON_VOLATILE |
                               EFI_VARIABLE_BOOTSERVICE_ACCESS |
                               EFI_VARIABLE_RUNTIME_ACCESS,
                           totalSize, buffer);
  BS->FreePool(buffer);
  return status;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE *loadedImage = NULL;
  EFI_HANDLE deviceHandle, disableImage;
  EFI_DEVICE_PATH_PROTOCOL *disablePath;
  UINT16 bootOrder[7] = {0x0002, 0x0006, 0x0005, 0x0004, 0x0003, 0x0000, 0x0001};
  UINT16 staleBootCurrent = 0x0000;

  ST = systemTable;
  BS = systemTable->BootServices;
  RT = systemTable->RuntimeServices;
  IM = image;
  (void)IM;

  Out(L"SetBootOrder: Setting up boot variables\r\n");

  if (EFI_ERROR(BS->HandleProtocol(image, &gEfiLoadedImageProtocolGuid,
                                   (void **)&loadedImage)) ||
      !loadedImage) {
    Out(L"Failed to get LoadedImage\r\n");
    return EFI_LOAD_ERROR;
  }
  deviceHandle = loadedImage->DeviceHandle;

  if (EFI_ERROR(CreateBootOption(0x0002, L"DisablePROCHOT",
                                 L"\\EFI\\BOOT\\DisablePROCHOT.efi", deviceHandle))) {
    Out(L"Failed to create Boot0002\r\n");
    return EFI_LOAD_ERROR;
  }
  Out(L"Created Boot0002 -> DisablePROCHOT.efi\r\n");

  CreateBootOption(0x0000, L"StaleCurrent", L"\\EFI\\BOOT\\Missing.efi", deviceHandle);
  Out(L"Created Boot0000 -> StaleCurrent\r\n");
  CreateBootOption(0x0001, L"WrongTarget", L"\\EFI\\BOOT\\WrongTarget.efi", deviceHandle);
  Out(L"Created Boot0001 -> WrongTarget.efi\r\n");
  CreateBootOption(0x0004, L"EFI USB Device", NULL, deviceHandle);
  Out(L"Created Boot0004 -> EFI USB Device\r\n");
  CreateBootOption(0x0005, L"Inactive", L"\\EFI\\BOOT\\Inactive.efi", deviceHandle);
  Out(L"Created Boot0005 -> Inactive\r\n");
  CreateBootOption(0x0006, L"Missing", L"\\EFI\\BOOT\\Missing.efi", deviceHandle);
  Out(L"Created Boot0006 -> Missing\r\n");
  CreateBootOption(0x0003, L"ChainSuccess", L"\\EFI\\BOOT\\ChainSuccess.efi", deviceHandle);
  Out(L"Created Boot0003 -> ChainSuccess.efi\r\n");

  status = RT->SetVariable(L"BootOrder", &gEfiGlobalVariableGuid,
                           EFI_VARIABLE_NON_VOLATILE |
                               EFI_VARIABLE_BOOTSERVICE_ACCESS |
                               EFI_VARIABLE_RUNTIME_ACCESS,
                           sizeof(bootOrder), bootOrder);
  if (EFI_ERROR(status)) {
    Out(L"Failed to set BootOrder\r\n");
    return status;
  }
  Out(L"Set BootOrder = {0002, 0006, 0005, 0004, 0003, 0000, 0001}\r\n");

  RT->SetVariable(L"BootCurrent", &gEfiGlobalVariableGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof(staleBootCurrent), &staleBootCurrent);
  Out(L"BootCurrent = 0000 (stale test value)\r\n");

  Out(L"Launching DisablePROCHOT.efi...\r\n");
  disablePath = FileDevicePath16(deviceHandle, L"\\EFI\\BOOT\\DisablePROCHOT.efi");
  if (!disablePath) {
    Out(L"Failed to create device path\r\n");
    return EFI_OUT_OF_RESOURCES;
  }
  status = BS->LoadImage(FALSE, image, disablePath, NULL, 0, &disableImage);
  if (EFI_ERROR(status)) {
    Out(L"Failed to load DisablePROCHOT.efi\r\n");
    return status;
  }
  return BS->StartImage(disableImage, NULL, NULL);
}
