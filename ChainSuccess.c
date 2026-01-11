// Minimal EFI app used for testing chainload functionality.
// Prints a success message and triggers system shutdown.
#include "efi.h"

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable);

// GNU-EFI entry point wrapper.
EFI_STATUS
_entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  return efi_main(image, systemTable);
}

// Print confirmation message and shut down the system.
EFI_STATUS
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systemTable) {
  SIMPLE_TEXT_OUTPUT_INTERFACE *conOut = systemTable->ConOut;
  conOut->OutputString(conOut, L"Chainload successful\r\n");
  conOut->OutputString(conOut, L"Shutting down\r\n");
  systemTable->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0,
                                            NULL);
  return EFI_SUCCESS;  // ResetSystem does not return
}
