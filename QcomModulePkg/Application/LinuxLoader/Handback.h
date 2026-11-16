/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GlassineOS hand-back: this bootloader as the parent of the GlassineOS loader
 * (GlassineOS/boot specs/012-device-boot-partition, docs/handback.md,
 * docs/boot-partition.md "The raw form", format 1).
 *
 * HandbackParse.c holds the checks of what is read from disk and from the
 * loader's exit data. It uses no library function and no firmware service, so
 * glassine/test-handback compiles it on the host and runs it against the
 * contract's cases. Handback.c is the firmware side.
 */
#ifndef __GLASSINE_HANDBACK_H__
#define __GLASSINE_HANDBACK_H__

/* The boot partition's raw form (docs/boot-partition.md, "The raw form"): the
 * region table, at the start and in the last 4096 bytes. */
#define HB_TABLE_SIZE        4096
#define HB_TABLE_ENTRIES_MAX 62
/* The region "loader" (docs/handback.md, "The loader region"): a 4096-byte
 * header, then the image. */
#define HB_HEADER_SIZE       4096
#define HB_IMAGE_MAX         ((3ULL << 20) - 4096)

/* The exit data: the UCS-2 prefix "GLASSINE-HANDBACK" and its zero, then at
 * most HB_ANSWER_MAX bytes of answer. */
#define HB_PREFIX_BYTES      36
#define HB_ANSWER_MAX        4096

typedef enum {
  HB_FLOOR = 0,  /* no valid answer: boot the baseline */
  HB_BASELINE,
  HB_RECOVERY,
  HB_SET,
} HB_BOOT;

typedef UINT32 (*HB_CRC32) (CONST UINT8 *Data, UINTN Size);

/* The region "loader" from one copy of the region table, which must be valid
 * as a whole for a partition of PartBytes bytes (magic, format, CRC, size, and
 * every entry: its CRC, name, alignment, bounds, no overlap, no repeat). FALSE:
 * this copy can't be used. */
BOOLEAN
HandbackFindLoader (CONST UINT8 *Table, UINT64 PartBytes, HB_CRC32 Crc,
                    UINT64 *Offset, UINT64 *Size);

/* The image size and its CRC-32 from the loader region's 4096-byte header, as
 * docs/handback.md, "The loader region", says. RegionBytes: the region's size.
 * FALSE: no loader. */
BOOLEAN
HandbackCheckHeader (CONST UINT8 *Header, UINT64 RegionBytes, HB_CRC32 Crc,
                     UINT64 *ImageSize, UINT32 *ImageCrc);

/* What the exit data asks for ("What the parent does with it"). Anything but
 * a whole, checked answer of format 1 with a known boot= is HB_FLOOR. For
 * HB_SET, Id gets the set's 16 hex digits and a zero. */
HB_BOOT
HandbackParse (CONST UINT8 *Data, UINTN Size, HB_CRC32 Crc, CHAR8 Id[17]);

/* The firmware side: on the GlassineOS slot, start the loader and read its
 * answer. TRUE only when the answer is the recovery; FALSE in every other
 * case, which is the baseline (boot_b, as without the loader). */
BOOLEAN
HandbackAsksRecovery (EFI_HANDLE ParentImage);

#endif
