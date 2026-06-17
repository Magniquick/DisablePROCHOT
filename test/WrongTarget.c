// EFI app used by tests to catch chainloading the wrong BootOrder entry.
#include <efi.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  (void)image;
  SIMPLE_TEXT_OUTPUT_INTERFACE *conOut = systemTable->ConOut;
  conOut->OutputString(conOut, L"Wrong chainload target\r\n");
  conOut->OutputString(conOut, L"Shutting down\r\n");
  systemTable->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0,
                                            NULL);
  return EFI_SUCCESS;
}
