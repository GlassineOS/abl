/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GlassineOS hand-back: the checks of the loader image header and of the
 * loader's answer (GlassineOS/boot docs/handback.md, format 1).
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

BOOLEAN
HandbackCheckHeader (CONST UINT8 *Header, UINT64 PartBytes, HB_CRC32 Crc,
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
  if (Size == 0 || Size > HB_IMAGE_MAX || PartBytes < HB_IMAGE_OFFSET ||
      Size > PartBytes - HB_IMAGE_OFFSET)
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
