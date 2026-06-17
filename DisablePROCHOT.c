// Copyright (c) 2018 Park Ju Hyung
//
// Self-contained variant: gnu-efi headers for TYPES only, no gnu-efi lib, so it
// can be compiled straight to PE-COFF with clang/lld (no ELF->PE objcopy).

#include <efi.h>

#ifndef LOAD_OPTION_ACTIVE
#define LOAD_OPTION_ACTIVE 0x00000001
#endif

// ---------------------------------------------------------------------------
// Minimal gnu-efi-lib replacements (so nothing external needs linking)
// ---------------------------------------------------------------------------
static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_RUNTIME_SERVICES *RT;
static EFI_HANDLE IM;

static EFI_GUID gEfiGlobalVariableGuid = EFI_GLOBAL_VARIABLE;
static EFI_GUID gEfiLoadedImageProtocolGuid = LOADED_IMAGE_PROTOCOL;
static EFI_GUID gEfiLoadedImageDevicePathProtocolGuid = {
    0xbc62157e, 0x3e33, 0x4fec, {0x99, 0x20, 0x2d, 0x3b, 0x36, 0xd7, 0x50, 0xdf}};
static EFI_GUID gEfiDevicePathProtocolGuid = {
    0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

static void Output(CHAR16 *s) {
#ifdef SILENT
  (void)s;  // no console output; LTO strips the now-unused string literals
#else
  ST->ConOut->OutputString(ST->ConOut, s);
#endif
}

static INTN CompareMem(const void *a, const void *b, UINTN n) {
  const UINT8 *x = a, *y = b;
  for (UINTN i = 0; i < n; i++)
    if (x[i] != y[i]) return (INTN)x[i] - (INTN)y[i];
  return 0;
}

static void CopyMem(void *d, const void *s, UINTN n) {
  UINT8 *dd = d;
  const UINT8 *ss = s;
  for (UINTN i = 0; i < n; i++) dd[i] = ss[i];
}

static void FreePool(void *p) { BS->FreePool(p); }

static UINTN DevicePathSize(EFI_DEVICE_PATH_PROTOCOL *dp) {
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

static EFI_DEVICE_PATH_PROTOCOL *FindFilePathNode(EFI_DEVICE_PATH_PROTOCOL *dp);

// This firmware's Boot#### entries are short-form (HD(sig)/File, no hardware
// prefix) and its LoadImage won't expand them (EFI_NOT_FOUND). Rebuild a FULL
// path: the device path of the partition we were loaded from + the target's
// File node + End. Returns NULL on failure (caller falls back to short-form).
static EFI_DEVICE_PATH_PROTOCOL *BuildFullPath(EFI_DEVICE_PATH_PROTOCOL *target) {
  EFI_LOADED_IMAGE *li = NULL;
  EFI_DEVICE_PATH_PROTOCOL *devDp = NULL, *targFile, *out;
  UINTN devLen, tlen;
  UINT8 *p;

  if (EFI_ERROR(BS->HandleProtocol(IM, &gEfiLoadedImageProtocolGuid, (void **)&li)) || !li)
    return NULL;
  if (EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &gEfiDevicePathProtocolGuid,
                                   (void **)&devDp)) ||
      !devDp)
    return NULL;
  targFile = FindFilePathNode(target);
  if (!targFile) return NULL;

  devLen = DevicePathSize(devDp);
  if (devLen < 4) return NULL;
  devLen -= 4;  // strip the partition path's End node
  tlen = (UINTN)DevicePathNodeLength(targFile);

  if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, devLen + tlen + 4, (void **)&out)) || !out)
    return NULL;
  p = (UINT8 *)out;
  CopyMem(p, devDp, devLen);             // hardware path to our partition
  CopyMem(p + devLen, targFile, tlen);   // target's File node
  p[devLen + tlen + 0] = 0x7f;           // End-of-device-path
  p[devLen + tlen + 1] = 0xff;
  p[devLen + tlen + 2] = 0x04;
  p[devLen + tlen + 3] = 0x00;
  return out;
}

static void *LibGetVariableAndSize(CHAR16 *name, EFI_GUID *g, UINTN *sz) {
  UINTN s = 0;
  *sz = 0;
  EFI_STATUS st = RT->GetVariable(name, g, NULL, &s, NULL);
  if (st != EFI_BUFFER_TOO_SMALL || s == 0) return NULL;
  void *d = NULL;
  if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, s, &d)) || !d) return NULL;
  if (EFI_ERROR(RT->GetVariable(name, g, NULL, &s, d))) {
    BS->FreePool(d);
    return NULL;
  }
  *sz = s;
  return d;
}

// ---------------------------------------------------------------------------
// MSR read/write helpers
// ---------------------------------------------------------------------------
static uint64_t AsmWriteMsr64(uint32_t index, uint64_t val) {
  uint32_t low = (uint32_t)(val);
  uint32_t high = (uint32_t)(val >> 32);
  __asm__ __volatile__("wrmsr" : : "c"(index), "a"(low), "d"(high) : "memory");
  return val;
}

static uint64_t AsmReadMsr64(uint32_t index) {
  uint32_t low, high;
  __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(index) : "memory");
  return ((uint64_t)high << 32) | low;
}

// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
  UINT32 Attributes;
  UINT16 FilePathListLength;
} EFI_LOAD_OPTION_HEADER;

static void MakeBootVarName(UINT16 bootId, CHAR16 name[9]) {
  const CHAR16 hex[] = L"0123456789ABCDEF";
  name[0] = L'B';
  name[1] = L'o';
  name[2] = L'o';
  name[3] = L't';
  name[4] = hex[(bootId >> 12) & 0xF];
  name[5] = hex[(bootId >> 8) & 0xF];
  name[6] = hex[(bootId >> 4) & 0xF];
  name[7] = hex[bootId & 0xF];
  name[8] = L'\0';
}

static EFI_DEVICE_PATH_PROTOCOL *ParseBootOption(UINT8 *bootData, UINTN bootSize);
static EFI_DEVICE_PATH_PROTOCOL *GetLoadedImagePath(void);
static BOOLEAN DevicePathsMatch(EFI_DEVICE_PATH_PROTOCOL *first,
                                EFI_DEVICE_PATH_PROTOCOL *second);

static EFI_STATUS GetNextBootOption(UINTN startOffset, UINT16 *nextBootId,
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

  bootOrderData =
      LibGetVariableAndSize(L"BootOrder", &gEfiGlobalVariableGuid, &bootOrderSize);
  if (!bootOrderData) return EFI_NOT_FOUND;

  if (bootOrderSize < sizeof(UINT16) || (bootOrderSize % sizeof(UINT16)) != 0) {
    FreePool(bootOrderData);
    return EFI_COMPROMISED_DATA;
  }

  bootOrder = (UINT16 *)bootOrderData;
  bootCount = bootOrderSize / sizeof(UINT16);

  loadedPath = GetLoadedImagePath();
  if (loadedPath) {
    for (i = 0; i < bootCount; ++i) {
      UINT16 bootId = bootOrder[i];
      CHAR16 bootVarName[9];
      UINT8 *bootData = NULL;
      UINTN bootSize = 0;
      EFI_DEVICE_PATH_PROTOCOL *devicePath;

      MakeBootVarName(bootId, bootVarName);
      bootData = LibGetVariableAndSize(bootVarName, &gEfiGlobalVariableGuid, &bootSize);
      if (!bootData) continue;

      devicePath = ParseBootOption(bootData, bootSize);
      if (devicePath && DevicePathsMatch(devicePath, loadedPath)) {
        found = TRUE;
        currentIndex = i;
        FreePool(bootData);
        break;
      }
      FreePool(bootData);
    }
  }

  if (!found) {
    UINTN size = sizeof(bootCurrent);
    status = RT->GetVariable(L"BootCurrent", &gEfiGlobalVariableGuid, NULL, &size,
                             &bootCurrent);
    if (EFI_ERROR(status)) bootCurrent = 0xFFFF;
    for (i = 0; i < bootCount; ++i) {
      if (bootOrder[i] == bootCurrent) {
        found = TRUE;
        currentIndex = i;
        break;
      }
    }
  }

  if (!found || bootCount == 1 || startOffset >= bootCount) {
    FreePool(bootOrderData);
    return EFI_NOT_FOUND;
  }

  for (i = startOffset; i < bootCount; ++i) {
    UINT16 bootId = bootOrder[(currentIndex + i) % bootCount];
    CHAR16 bootVarName[9];
    UINT8 *bootData = NULL;
    UINTN bootSize = 0;

    MakeBootVarName(bootId, bootVarName);
    bootData = LibGetVariableAndSize(bootVarName, &gEfiGlobalVariableGuid, &bootSize);
    if (!bootData) continue;

    if (ParseBootOption(bootData, bootSize)) {
      *nextBootId = bootId;
      *nextOffset = i;
      FreePool(bootData);
      FreePool(bootOrderData);
      return EFI_SUCCESS;
    }
    FreePool(bootData);
  }

  FreePool(bootOrderData);
  return EFI_NOT_FOUND;
}

static EFI_DEVICE_PATH_PROTOCOL *GetLoadedImagePath(void) {
  EFI_DEVICE_PATH_PROTOCOL *devicePath = NULL;
  EFI_LOADED_IMAGE *loadedImage = NULL;

  if (!EFI_ERROR(BS->HandleProtocol(IM, &gEfiLoadedImageDevicePathProtocolGuid,
                                    (void **)&devicePath)) &&
      devicePath)
    return devicePath;

  if (!EFI_ERROR(BS->HandleProtocol(IM, &gEfiLoadedImageProtocolGuid,
                                    (void **)&loadedImage)) &&
      loadedImage)
    return loadedImage->FilePath;

  return NULL;
}

static EFI_DEVICE_PATH_PROTOCOL *FindFilePathNode(EFI_DEVICE_PATH_PROTOCOL *dp) {
  EFI_DEVICE_PATH_PROTOCOL *node = dp;
  while (node && !IsDevicePathEnd(node)) {
    if (DevicePathType(node) == MEDIA_DEVICE_PATH &&
        DevicePathSubType(node) == MEDIA_FILEPATH_DP)
      return node;
    node = NextDevicePathNode(node);
  }
  return NULL;
}

static BOOLEAN DevicePathHasFilePath(EFI_DEVICE_PATH_PROTOCOL *dp) {
  return FindFilePathNode(dp) != NULL;
}

static BOOLEAN DevicePathsMatch(EFI_DEVICE_PATH_PROTOCOL *first,
                                EFI_DEVICE_PATH_PROTOCOL *second) {
  UINTN firstSize, secondSize;
  EFI_DEVICE_PATH_PROTOCOL *firstFile, *secondFile;

  if (!first || !second) return FALSE;

  firstSize = DevicePathSize(first);
  secondSize = DevicePathSize(second);
  if (firstSize && firstSize == secondSize &&
      CompareMem(first, second, firstSize) == 0)
    return TRUE;

  firstFile = FindFilePathNode(first);
  secondFile = FindFilePathNode(second);
  if (!firstFile || !secondFile) return FALSE;

  firstSize = (UINTN)DevicePathNodeLength(firstFile);
  secondSize = (UINTN)DevicePathNodeLength(secondFile);
  return firstSize == secondSize &&
         CompareMem(firstFile, secondFile, firstSize) == 0;
}

static EFI_DEVICE_PATH_PROTOCOL *ParseBootOption(UINT8 *bootData, UINTN bootSize) {
  EFI_LOAD_OPTION_HEADER *header;
  UINT8 *descPtr;
  CHAR16 *description;
  UINTN descMax, descLen;
  UINT8 *filePath;
  EFI_DEVICE_PATH_PROTOCOL *devicePath;

  if (bootSize < sizeof(EFI_LOAD_OPTION_HEADER) + sizeof(CHAR16)) return NULL;

  header = (EFI_LOAD_OPTION_HEADER *)bootData;
  if ((header->Attributes & LOAD_OPTION_ACTIVE) == 0) return NULL;

  descPtr = bootData + sizeof(EFI_LOAD_OPTION_HEADER);
  description = (CHAR16 *)descPtr;

  descMax = (bootSize - (UINTN)(descPtr - bootData)) / sizeof(CHAR16);
  for (descLen = 0; descLen < descMax; ++descLen)
    if (description[descLen] == L'\0') break;
  if (descLen == descMax) return NULL;

  filePath = (UINT8 *)(description + descLen + 1);
  if ((UINTN)(filePath - bootData) + header->FilePathListLength > bootSize ||
      header->FilePathListLength == 0)
    return NULL;

  devicePath = (EFI_DEVICE_PATH_PROTOCOL *)filePath;
  if (!DevicePathHasFilePath(devicePath)) return NULL;

  return devicePath;
}

static EFI_STATUS TryBootOrderChainload(void) {
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
    status = GetNextBootOption(offset, &nextBootId, &nextOffset);
    if (status == EFI_NOT_FOUND) {
      Output(L"No next boot entry\r\n");
      return status;
    }
    if (EFI_ERROR(status)) {
      Output(L"BootOrder unavailable\r\n");
      return status;
    }
    offset = nextOffset + 1;

    MakeBootVarName(nextBootId, bootVarName);
    bootData = LibGetVariableAndSize(bootVarName, &gEfiGlobalVariableGuid, &bootSize);
    if (!bootData) continue;

    devicePath = ParseBootOption(bootData, bootSize);
    if (!devicePath) {
      FreePool(bootData);
      continue;
    }

    Output(L"Chainloading next boot entry\r\n");
    // Expand the short-form boot path to a full path off our own partition;
    // fall back to the raw path if that fails (e.g. firmware that expands it).
    EFI_DEVICE_PATH_PROTOCOL *full = BuildFullPath(devicePath);
    status = BS->LoadImage(FALSE, IM, full ? full : devicePath, NULL, 0, &nextImage);
    if (full) FreePool(full);
    FreePool(bootData);
    bootData = NULL;
    if (EFI_ERROR(status)) continue;

    status = BS->StartImage(nextImage, NULL, NULL);
    if (EFI_ERROR(status)) continue;

    return status;
  }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  ST = systemTable;
  BS = systemTable->BootServices;
  RT = systemTable->RuntimeServices;
  IM = image;

  Output(L"Disabling BD PROCHOT + VR Thermal Alert\r\n");
  uint64_t powerCtl = AsmReadMsr64(0x1FC);
  powerCtl &= ~(uint64_t)0x1;
  powerCtl |= (uint64_t)0x1 << 24;
  AsmWriteMsr64(0x1FC, powerCtl);
  Output(L"BD PROCHOT + VR Thermal Alert disabled\r\n");

  return TryBootOrderChainload();
}
