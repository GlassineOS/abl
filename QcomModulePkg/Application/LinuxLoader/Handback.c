/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GlassineOS hand-back: on the GlassineOS slot, start the GlassineOS loader
 * from the boot partition and act on its answer (GlassineOS/boot
 * specs/012-device-boot-partition, docs/handback.md, docs/boot-partition.md
 * "The raw form", format 1). Only the raw form is read: the region table
 * (primary, else backup), then the region "loader". A partition in the FAT
 * form, or any doubt about the table, is the floor.
 *
 * The floor: every failure on the way returns HB_FLOOR, and the caller boots
 * boot_b exactly as without this code. No register is touched (no MMIO), no
 * storage is written, nothing waits without an end except the loader itself,
 * which runs under a 30 s firmware watchdog.
 *
 * What the answer can do here: "recovery" sets the recovery boot (the
 * unsuffixed recovery partition, as a recovery request does). "other:android"
 * boots the Android slot (boot_a) at this start only; the caller does that
 * without writing a slot attribute (LinuxLoader.c). "baseline" and every
 * failure boot boot_b. "set:<id>" names a kernel set from the block-list
 * hand-off (GlassineOS/boot docs/blocklist.md); this build has no block-list
 * reader yet, so it is logged and boot_b boots. cmdline_add is ignored.
 */
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/LinuxLoaderLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Uefi.h>

#include "Handback.h"

/* The boot partition, found by its GPT name. On a table without it the loader
 * is never started. */
STATIC CHAR16 HandbackPartName[] = L"glassine-boot";
STATIC CHAR16 HandbackOptions[] = HB_LOAD_OPTIONS;

#define HB_WATCHDOG_SECONDS 30

STATIC UINT32
FwCrc32 (CONST UINT8 *Data, UINTN Size)
{
  UINT32 Crc = 0;

  if (gBS->CalculateCrc32 ((VOID *)Data, Size, &Crc) != EFI_SUCCESS)
    return ~(UINT32)0; /* Never matches a stored CRC of real data by chance alone. */
  return Crc;
}

/* Reads Size bytes at byte offset Offset into a page buffer it allocates.
 * The read is rounded up to whole blocks. NULL on any failure. */
STATIC VOID *
ReadAt (EFI_BLOCK_IO_PROTOCOL *BlkIo, UINT64 Offset, UINT64 Size,
        UINTN *Pages)
{
  UINT32 Bs = BlkIo->Media->BlockSize;
  UINT64 Len;
  VOID *Buf;

  if (Bs == 0 || (Offset % Bs) != 0 || Size == 0)
    return NULL;
  Len = ((Size + Bs - 1) / Bs) * Bs;
  *Pages = EFI_SIZE_TO_PAGES ((UINTN)Len);
  Buf = AllocatePages (*Pages);
  if (Buf == NULL)
    return NULL;
  if (BlkIo->ReadBlocks (BlkIo, BlkIo->Media->MediaId, Offset / Bs,
                         (UINTN)Len, Buf) != EFI_SUCCESS) {
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Buf, *Pages);
    return NULL;
  }
  return Buf;
}

HB_BOOT
HandbackAsk (EFI_HANDLE ParentImage)
{
  PartiSelectFilter Filter;
  HandleInfo Info[1];
  UINT32 Count = 1;
  EFI_BLOCK_IO_PROTOCOL *BlkIo;
  UINT64 PartBytes;
  VOID *Table = NULL;
  UINTN TablePages = 0;
  UINT64 RegionOff = 0;
  UINT64 RegionSize = 0;
  VOID *Header = NULL;
  UINTN HeaderPages = 0;
  VOID *Image = NULL;
  UINTN ImagePages = 0;
  UINT64 ImageSize = 0;
  UINT32 ImageCrc = 0;
  EFI_HANDLE Child = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Li = NULL;
  UINTN ExitSize = 0;
  CHAR16 *ExitData = NULL;
  EFI_STATUS Status;
  HB_BOOT Answer = HB_FLOOR;
  CHAR8 Id[17] = {0};

  Filter.RootDeviceType = NULL;
  Filter.PartitionLabel = HandbackPartName;
  Filter.VolumeName = NULL;
  /* The selection LoadImageFromPartition uses for boot partitions. */
  Status = GetBlkIOHandles (BLK_IO_SEL_PARTITIONED_MBR |
                            BLK_IO_SEL_PARTITIONED_GPT |
                            BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE |
                            BLK_IO_SEL_MATCH_PARTITION_LABEL,
                            &Filter, Info, &Count);
  if (Status != EFI_SUCCESS || Count != 1) {
    DEBUG ((EFI_D_INFO, "glassine: no boot partition (%r, %u): boot_b\n",
            Status, Count));
    return HB_FLOOR;
  }
  BlkIo = Info[0].BlkIo;
  if (BlkIo == NULL || BlkIo->Media == NULL || !BlkIo->Media->MediaPresent ||
      BlkIo->Media->BlockSize == 0 || (4096 % BlkIo->Media->BlockSize) != 0) {
    DEBUG ((EFI_D_ERROR, "glassine: boot partition unusable: boot_b\n"));
    return HB_FLOOR;
  }
  PartBytes = (BlkIo->Media->LastBlock + 1) * (UINT64)BlkIo->Media->BlockSize;
  if (PartBytes < 2 * HB_TABLE_SIZE) {
    DEBUG ((EFI_D_ERROR, "glassine: boot partition too small: boot_b\n"));
    return HB_FLOOR;
  }

  /* The region table: the primary, else the backup in the last 4096 bytes. */
  Table = ReadAt (BlkIo, 0, HB_TABLE_SIZE, &TablePages);
  if (Table == NULL || !HandbackFindLoader (Table, PartBytes, FwCrc32,
                                            &RegionOff, &RegionSize)) {
    if (Table != NULL)
      gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Table, TablePages);
    Table = ReadAt (BlkIo, PartBytes - HB_TABLE_SIZE, HB_TABLE_SIZE,
                    &TablePages);
    if (Table == NULL || !HandbackFindLoader (Table, PartBytes, FwCrc32,
                                              &RegionOff, &RegionSize)) {
      DEBUG ((EFI_D_INFO, "glassine: no region table with a loader on the "
                          "boot partition: boot_b\n"));
      goto out;
    }
  }

  Header = ReadAt (BlkIo, RegionOff, HB_HEADER_SIZE, &HeaderPages);
  if (Header == NULL ||
      !HandbackCheckHeader (Header, RegionSize, FwCrc32, &ImageSize,
                            &ImageCrc)) {
    DEBUG ((EFI_D_INFO, "glassine: no loader in the loader region: boot_b\n"));
    goto out;
  }
  Image = ReadAt (BlkIo, RegionOff + HB_HEADER_SIZE, ImageSize, &ImagePages);
  if (Image == NULL || FwCrc32 (Image, (UINTN)ImageSize) != ImageCrc) {
    DEBUG ((EFI_D_ERROR, "glassine: loader unreadable or its CRC fails: "
                         "boot_b\n"));
    goto out;
  }

  /* With the partition's device path: an EDK2 with image verification refuses
   * a buffer with none (docs/handback.md, step 2). Nothing is read through it. */
  Status = gBS->LoadImage (FALSE, ParentImage,
                           DevicePathFromHandle (Info[0].Handle), Image,
                           (UINTN)ImageSize, &Child);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "glassine: LoadImage: %r: boot_b\n", Status));
    if (Child != NULL)
      gBS->UnloadImage (Child); /* EFI_SECURITY_VIOLATION leaves a handle */
    goto out;
  }
  Status = gBS->HandleProtocol (Child, &gEfiLoadedImageProtocolGuid,
                                (VOID **)&Li);
  if (EFI_ERROR (Status) || Li == NULL) {
    gBS->UnloadImage (Child);
    goto out;
  }
  Li->DeviceHandle = Info[0].Handle;
  Li->LoadOptions = HandbackOptions;
  Li->LoadOptionsSize = sizeof (HandbackOptions);

  DEBUG ((EFI_D_INFO, "glassine: starting the loader (%llu bytes)\n",
          ImageSize));
  gBS->SetWatchdogTimer (HB_WATCHDOG_SECONDS, 0, 0, NULL);
  Status = gBS->StartImage (Child, &ExitSize, &ExitData);
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  /* An application is unloaded when it returns: Child is gone now. */

  Answer = HandbackParse ((CONST UINT8 *)ExitData, ExitSize, FwCrc32, Id);
  /* gBS directly: the MemoryAllocationLib wrappers ASSERT on a failed free,
   * and in this DEBUG build an ASSERT is a hang. The status is ignored. */
  if (ExitData != NULL)
    gBS->FreePool (ExitData);
  switch (Answer) {
  case HB_RECOVERY:
    DEBUG ((EFI_D_INFO, "glassine: the loader asks for the recovery\n"));
    break;
  case HB_ANDROID:
    DEBUG ((EFI_D_INFO, "glassine: the loader asks for Android, once\n"));
    break;
  case HB_BASELINE:
    DEBUG ((EFI_D_INFO, "glassine: the loader asks for boot_b\n"));
    break;
  case HB_SET:
    DEBUG ((EFI_D_INFO, "glassine: the loader asks for set %a; no block-list "
                        "reader in this build: boot_b\n", Id));
    break;
  default:
    DEBUG ((EFI_D_ERROR, "glassine: no valid answer (%r, %u bytes): boot_b\n",
            Status, ExitSize));
    break;
  }

out:
  if (Image != NULL)
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Image, ImagePages);
  if (Header != NULL)
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Header, HeaderPages);
  if (Table != NULL)
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Table, TablePages);
  return Answer;
}
