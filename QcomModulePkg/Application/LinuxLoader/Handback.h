/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GlassineOS hand-back: this bootloader as the parent of the GlassineOS loader
 * (GlassineOS/boot specs/012-handback, docs/handback.md, format 1).
 *
 * HandbackParse.c holds the checks of what is read from disk and from the
 * loader's exit data. It uses no library function and no firmware service, so
 * glassine/test-handback compiles it on the host and runs it against the
 * contract's cases. Handback.c is the firmware side.
 */
#ifndef __GLASSINE_HANDBACK_H__
#define __GLASSINE_HANDBACK_H__

/* The hand-back partition (docs/handback.md, "The hand-back partition"). */
#define HB_PART_MIN          (4ULL << 20)
#define HB_HEADER_OFFSET     (1ULL << 20)
#define HB_HEADER_SIZE       4096
#define HB_IMAGE_OFFSET      (HB_HEADER_OFFSET + HB_HEADER_SIZE)
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

/* The image size and its CRC-32 from the 4096-byte loader image header, as
 * docs/handback.md, "The loader image header", says. FALSE: no loader. */
BOOLEAN
HandbackCheckHeader (CONST UINT8 *Header, UINT64 PartBytes, HB_CRC32 Crc,
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
