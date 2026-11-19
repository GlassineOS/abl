/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GlassineOS hand-back: the checks of the boot partition's region table, of
 * the loader region's header and of the loader's answer (GlassineOS/boot
 * docs/boot-partition.md "The raw form", docs/handback.md, format 1).
 *
 * Everything here reads bytes that came from disk or from another program, so
 * every length is bounded by the caller's size and nothing assumes a zero at
 * the end. No library function is called (the BaseLib string functions ASSERT
 * on bad input in a DEBUG build, and an ASSERT is a hang).
 */
#ifdef GLASSINE_HOST_TEST
#include "HostTypes.h"
#else
#include <Uefi.h>
#endif
#include "Handback.h"

STATIC UINT32
Le32 (CONST UINT8 *P)
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) |
         ((UINT32)P[3] << 24);
}

STATIC UINT64
Le64 (CONST UINT8 *P)
{
  return (UINT64)Le32 (P) | ((UINT64)Le32 (P + 4) << 32);
}

STATIC BOOLEAN
IsNameChar (UINT8 C)
{
  return (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '-';
}

BOOLEAN
HandbackFindLoader (CONST UINT8 *Table, UINT64 PartBytes, HB_CRC32 Crc,
                    UINT64 *Offset, UINT64 *Size)
{
  STATIC CONST UINT8 Magic[8] = {'G', 'L', 'R', 'E', 'G', 'I', 'O', 'N'};
  STATIC CONST UINT8 Loader[7] = {'l', 'o', 'a', 'd', 'e', 'r', 0};
  UINTN I;
  UINTN J;
  UINTN K;
  UINTN Count;
  UINT64 End;
  BOOLEAN Found = FALSE;

  for (I = 0; I < 8; I++) {
    if (Table[I] != Magic[I])
      return FALSE;
  }
  if (Crc (Table, 4092) != Le32 (Table + 4092))
    return FALSE;
  if (Le32 (Table + 8) != 1 || Le64 (Table + 16) != PartBytes ||
      PartBytes < 2 * HB_TABLE_SIZE)
    return FALSE;
  Count = Le32 (Table + 12);
  if (Count == 0 || Count > HB_TABLE_ENTRIES_MAX)
    return FALSE;
  for (I = 24; I < 64; I++) {
    if (Table[I] != 0)
      return FALSE;
  }
  for (I = 64 + 64 * Count; I < 4092; I++) {
    if (Table[I] != 0)
      return FALSE;
  }
  End = PartBytes - HB_TABLE_SIZE;
  for (I = 0; I < Count; I++) {
    CONST UINT8 *E = Table + 64 + 64 * I;
    UINT64 Off = Le64 (E + 32);
    UINT64 Sz = Le64 (E + 40);
    UINTN Len = 0;

    if (Crc (E, 52) != Le32 (E + 52))
      return FALSE;
    for (K = 56; K < 64; K++) {
      if (E[K] != 0)
        return FALSE;
    }
    while (Len < 32 && E[Len] != 0)
      Len++;
    if (Len == 0 || Len > 31)
      return FALSE;
    for (K = 0; K < Len; K++) {
      if (!IsNameChar (E[K]))
        return FALSE;
    }
    for (K = Len; K < 32; K++) {
      if (E[K] != 0)
        return FALSE;
    }
    if ((Off % 4096) != 0 || (Sz % 4096) != 0 || Sz == 0 ||
        Off < HB_TABLE_SIZE || Off > End || Sz > End - Off)
      return FALSE;
    for (J = 0; J < I; J++) {
      CONST UINT8 *F = Table + 64 + 64 * J;
      UINT64 FOff = Le64 (F + 32);
      UINT64 FSz = Le64 (F + 40);
      BOOLEAN SameName = TRUE;

      for (K = 0; K < 32; K++) {
        if (E[K] != F[K]) {
          SameName = FALSE;
          break;
        }
      }
      if (SameName || (Off < FOff + FSz && FOff < Off + Sz))
        return FALSE;
    }
    /* "loader", with no flag set. */
    if (Len == 6 && Le32 (E + 48) == 0) {
      BOOLEAN IsLoader = TRUE;
      for (K = 0; K < 7; K++) {
        if (E[K] != Loader[K]) {
          IsLoader = FALSE;
          break;
        }
      }
      if (IsLoader) {
        *Offset = Off;
        *Size = Sz;
        Found = TRUE;
      }
    }
  }
  return Found;
}

BOOLEAN
HandbackCheckHeader (CONST UINT8 *Header, UINT64 RegionBytes, HB_CRC32 Crc,
                     UINT64 *ImageSize, UINT32 *ImageCrc)
{
  STATIC CONST UINT8 Magic[8] = {'G', 'L', 'L', 'O', 'A', 'D', 'E', 'R'};
  UINTN I;
  UINT64 Size;

  for (I = 0; I < 8; I++) {
    if (Header[I] != Magic[I])
      return FALSE;
  }
  if (Crc (Header, 28) != Le32 (Header + 28))
    return FALSE;
  if (Le32 (Header + 8) != 1 || Le32 (Header + 12) != 0)
    return FALSE;
  Size = Le64 (Header + 16);
  if (Size == 0 || Size > HB_IMAGE_MAX || RegionBytes < HB_HEADER_SIZE ||
      Size > RegionBytes - HB_HEADER_SIZE)
    return FALSE;
  *ImageSize = Size;
  *ImageCrc = Le32 (Header + 24);
  return TRUE;
}

/* TRUE when Line[0..Len) is Key followed by '='. */
STATIC BOOLEAN
IsKey (CONST UINT8 *Line, UINTN Len, CONST CHAR8 *Key, UINTN KeyLen)
{
  UINTN I;

  if (Len < KeyLen + 1 || Line[KeyLen] != '=')
    return FALSE;
  for (I = 0; I < KeyLen; I++) {
    if (Line[I] != (UINT8)Key[I])
      return FALSE;
  }
  return TRUE;
}

STATIC BOOLEAN
Same (CONST UINT8 *A, UINTN Len, CONST CHAR8 *B, UINTN BLen)
{
  UINTN I;

  if (Len != BLen)
    return FALSE;
  for (I = 0; I < Len; I++) {
    if (A[I] != (UINT8)B[I])
      return FALSE;
  }
  return TRUE;
}

STATIC BOOLEAN
IsLowerHex (UINT8 C)
{
  return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
}

HB_BOOT
HandbackParse (CONST UINT8 *Data, UINTN Size, HB_CRC32 Crc, CHAR8 Id[17])
{
  STATIC CONST CHAR8 Prefix[] = "GLASSINE-HANDBACK";
  CONST UINT8 *A;
  UINTN Len;
  UINTN I;
  UINTN Start;
  UINTN BodyEnd;
  UINTN LineNo;
  UINT32 Want;
  CONST UINT8 *Boot = NULL;
  UINTN BootLen = 0;
  UINTN Boots = 0;

  if (Data == NULL || Size < HB_PREFIX_BYTES ||
      Size > HB_PREFIX_BYTES + HB_ANSWER_MAX)
    return HB_FLOOR;
  /* The UCS-2 prefix and its zero. */
  for (I = 0; I < 17; I++) {
    if (Data[2 * I] != (UINT8)Prefix[I] || Data[2 * I + 1] != 0)
      return HB_FLOOR;
  }
  if (Data[34] != 0 || Data[35] != 0)
    return HB_FLOOR;

  A = Data + HB_PREFIX_BYTES;
  Len = Size - HB_PREFIX_BYTES;
  /* Text: ends with a newline; no zero byte, no carriage return. */
  if (Len < 2 || A[Len - 1] != '\n')
    return HB_FLOOR;
  for (I = 0; I < Len; I++) {
    if (A[I] == 0 || A[I] == '\r')
      return HB_FLOOR;
  }

  /* The last line: crc32=<8 lower-case hex digits> over every byte before it. */
  BodyEnd = 0;
  for (I = Len - 1; I > 0; I--) {
    if (A[I - 1] == '\n') {
      BodyEnd = I;
      break;
    }
  }
  if (Len - BodyEnd != 15 || !IsKey (A + BodyEnd, 15, "crc32", 5))
    return HB_FLOOR;
  Want = 0;
  for (I = 0; I < 8; I++) {
    UINT8 C = A[BodyEnd + 6 + I];
    if (!IsLowerHex (C))
      return HB_FLOOR;
    Want = (Want << 4) | (UINT32)(C <= '9' ? C - '0' : C - 'a' + 10);
  }
  if (Crc (A, BodyEnd) != Want)
    return HB_FLOOR;

  /* The lines before it: format=1 first, then key=value; boot= exactly once.
   * Other keys are not ours to check (a key twice is the loader's bug, and
   * only boot= decides anything here). */
  Start = 0;
  LineNo = 0;
  for (I = 0; I < BodyEnd; I++) {
    if (A[I] != '\n')
      continue;
    if (LineNo == 0 && !Same (A + Start, I - Start, "format=1", 8))
      return HB_FLOOR;
    if (IsKey (A + Start, I - Start, "boot", 4)) {
      Boot = A + Start + 5;
      BootLen = I - Start - 5;
      Boots++;
    }
    LineNo++;
    Start = I + 1;
  }
  if (LineNo == 0 || Boots != 1)
    return HB_FLOOR;

  if (Same (Boot, BootLen, "baseline", 8))
    return HB_BASELINE;
  if (Same (Boot, BootLen, "recovery", 8))
    return HB_RECOVERY;
  /* other:<name>, the parent's other system: only the one this parent
   * announces (HB_LOAD_OPTIONS). Any other name is not a form it knows. */
  if (Same (Boot, BootLen, "other:android", 13))
    return HB_ANDROID;
  if (BootLen == 20 && Same (Boot, 4, "set:", 4)) {
    for (I = 0; I < 16; I++) {
      if (!IsLowerHex (Boot[4 + I]))
        return HB_FLOOR;
      Id[I] = (CHAR8)Boot[4 + I];
    }
    Id[16] = 0;
    return HB_SET;
  }
  return HB_FLOOR;
}
