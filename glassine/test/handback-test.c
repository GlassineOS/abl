/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Host test of HandbackParse.c: the loader image header and the answer, the
 * cases of GlassineOS/boot docs/handback.md ("What the parent does with it").
 * Run by glassine/test-handback.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "HostTypes.h"
#include "Handback.h"

static UINT32 Crc32 (CONST UINT8 *P, UINTN N)
{
  UINT32 C = 0xFFFFFFFFu;
  for (UINTN I = 0; I < N; I++) {
    C ^= P[I];
    for (int K = 0; K < 8; K++)
      C = (C >> 1) ^ (0xEDB88320u & (0u - (C & 1)));
  }
  return ~C;
}

static int Fails;
#define CHECK(c, ...) do { if (!(c)) { Fails++; printf ("FAIL %d: ", __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)

/* The exit data: the UCS-2 prefix, then Text. */
static UINTN Data (UINT8 *Out, CONST char *Text)
{
  CONST char *P = "GLASSINE-HANDBACK";
  UINTN N = 0;
  for (; *P; P++) { Out[N++] = (UINT8)*P; Out[N++] = 0; }
  Out[N++] = 0; Out[N++] = 0;
  memcpy (Out + N, Text, strlen (Text));
  return N + strlen (Text);
}

/* Text with its crc32 line. */
static char *WithCrc (CONST char *Body)
{
  static char B[8192];
  snprintf (B, sizeof B, "%scrc32=%08x\n", Body, Crc32 ((CONST UINT8 *)Body, strlen (Body)));
  return B;
}

static HB_BOOT Parse (CONST char *Text, char Id[17])
{
  static UINT8 D[16384];
  UINTN N = Data (D, Text);
  return HandbackParse (D, N, Crc32, Id);
}

int main (void)
{
  char Id[17];
  UINT8 D[16384];
  UINTN N;

  CHECK (Crc32 ((CONST UINT8 *)"123456789", 9) == 0xcbf43926u, "crc32 is not zlib's");

  /* The bytes GlassineOS/loader core/src/answer.rs checks too (tests::SHARED):
   * what its Answer::text writes for a limit and for a requested set. */
  {
    static CONST char *Shared[] = {
      "format=1\nboot=recovery\nwhy=limit\ncrc32=a84aa7ed\n",
      "format=1\nboot=set:0123456789abcdef\nset_rec=\nwhy=request\ncrc32=1fee18ad\n",
    };
    CHECK (Parse (Shared[0], Id) == HB_RECOVERY, "shared 0");
    CHECK (Parse (Shared[1], Id) == HB_SET && strcmp (Id, "0123456789abcdef") == 0, "shared 1");
  }
  /* A key other than boot= twice: the contract lets a parent not check keys it
   * doesn't use; this one doesn't. */
  CHECK (Parse (WithCrc ("format=1\nwhy=a\nwhy=b\nboot=recovery\n"), Id) == HB_RECOVERY, "why twice");

  /* Answers the loader writes (core/src/answer.rs, Answer::text). */
  CHECK (Parse (WithCrc ("format=1\nboot=baseline\nwhy=default\n"), Id) == HB_BASELINE, "baseline");
  CHECK (Parse (WithCrc ("format=1\nboot=recovery\nwhy=limit\n"), Id) == HB_RECOVERY, "recovery");
  CHECK (Parse (WithCrc ("format=1\nboot=set:0123456789abcdef\nset_rec=\nwhy=request\n"), Id) == HB_SET &&
         strcmp (Id, "0123456789abcdef") == 0, "set");
  CHECK (Parse (WithCrc ("format=1\nfuture=1\nboot=recovery\ncmdline_add=glassine.deploy.once=x\n"), Id) == HB_RECOVERY,
         "unknown keys are ignored");

  /* Everything else is the floor. */
  CHECK (HandbackParse (NULL, 0, Crc32, Id) == HB_FLOOR, "no data");
  N = Data (D, WithCrc ("format=1\nboot=recovery\n"));
  CHECK (HandbackParse (D, 20, Crc32, Id) == HB_FLOOR, "short");
  CHECK (HandbackParse (D, 35, Crc32, Id) == HB_FLOOR, "prefix only, cut");
  D[0] = 'X';
  CHECK (HandbackParse (D, N, Crc32, Id) == HB_FLOOR, "prefix");
  N = Data (D, WithCrc ("format=1\nboot=recovery\n"));
  D[2 * 17] = 1;
  CHECK (HandbackParse (D, N, Crc32, Id) == HB_FLOOR, "prefix zero");
  N = Data (D, WithCrc ("format=1\nboot=recovery\n"));
  D[36 + 14] ^= 1;
  CHECK (HandbackParse (D, N, Crc32, Id) == HB_FLOOR, "a flipped byte");
  {
    static UINT8 Big[36 + 4097 + 64];
    UINTN M = Data (Big, WithCrc ("format=1\nboot=recovery\n"));
    memset (Big + M, '\n', sizeof Big - M);
    CHECK (HandbackParse (Big, 36 + 4097, Crc32, Id) == HB_FLOOR, "too long");
  }
  CHECK (Parse ("format=1\nboot=recovery\n", Id) == HB_FLOOR, "no crc line");
  CHECK (Parse ("format=1\nboot=recovery\ncrc32=00000000\n", Id) == HB_FLOOR, "wrong crc");
  {
    char B[256];
    snprintf (B, sizeof B, "format=1\nboot=recovery\ncrc32=%08X\n", Crc32 ((CONST UINT8 *)"format=1\nboot=recovery\n", 23));
    CHECK (Parse (B, Id) == HB_FLOOR || strspn (B + 30, "0123456789") == 8, "upper-case crc");
  }
  CHECK (Parse (WithCrc ("format=2\nboot=recovery\n"), Id) == HB_FLOOR, "format 2");
  CHECK (Parse (WithCrc ("boot=recovery\nformat=1\n"), Id) == HB_FLOOR, "format not first");
  CHECK (Parse (WithCrc ("format=1\nboot=recovery\nboot=recovery\n"), Id) == HB_FLOOR, "boot twice");
  CHECK (Parse (WithCrc ("format=1\nwhy=x\n"), Id) == HB_FLOOR, "no boot");
  CHECK (Parse (WithCrc ("format=1\n"), Id) == HB_FLOOR, "only format");
  {
    CONST char *Bad[] = {"reboot", "set:", "set:0123", "set:0123456789ABCDEF", "set:0123456789abcdefg",
                         "set:0123456789abcde", "Baseline", "", "recovery ", "baseline\t"};
    for (UINTN I = 0; I < sizeof Bad / sizeof *Bad; I++) {
      char B[128];
      snprintf (B, sizeof B, "format=1\nboot=%s\n", Bad[I]);
      CHECK (Parse (WithCrc (B), Id) == HB_FLOOR, "boot=%s", Bad[I]);
    }
  }
  CHECK (Parse (WithCrc ("format=1\nboot=recovery\r\n"), Id) == HB_FLOOR, "carriage return");
  N = Data (D, WithCrc ("format=1\nboot=recovery\n"));
  D[N++] = 0;
  CHECK (HandbackParse (D, N, Crc32, Id) == HB_FLOOR, "a zero after the answer");
  N = Data (D, WithCrc ("format=1\nboot=recovery\n"));
  CHECK (HandbackParse (D, N - 1, Crc32, Id) == HB_FLOOR, "no final newline");
  /* Every cut of a good answer is the floor. */
  N = Data (D, WithCrc ("format=1\nboot=recovery\nwhy=limit\n"));
  for (UINTN Cut = 0; Cut < N; Cut++)
    CHECK (HandbackParse (D, Cut, Crc32, Id) == HB_FLOOR, "cut at %zu", Cut);
  /* Random bytes never ask for the recovery. */
  srand (1);
  for (int R = 0; R < 200000; R++) {
    UINTN M = 36 + (UINTN)(rand () % 200);
    Data (D, "");
    for (UINTN I = 36; I < M; I++)
      D[I] = (UINT8)(rand () % 3 == 0 ? '\n' : rand ());
    CHECK (HandbackParse (D, M, Crc32, Id) == HB_FLOOR, "random %d", R);
  }

  /* The loader image header (core/src/raw.rs, loader_header). */
  {
    UINT8 H[4096] = {0};
    UINT64 Size;
    UINT32 Crc;
    UINT64 Part = 8ull << 20;
    UINT32 Img = 0x12345678u;
    memcpy (H, "GLLOADER", 8);
    H[8] = 1;
    UINT64 S = 365568;
    for (int I = 0; I < 8; I++) H[16 + I] = (UINT8)(S >> (8 * I));
    for (int I = 0; I < 4; I++) H[24 + I] = (UINT8)(Img >> (8 * I));
    UINT32 Hc = Crc32 (H, 28);
    for (int I = 0; I < 4; I++) H[28 + I] = (UINT8)(Hc >> (8 * I));
    CHECK (HandbackCheckHeader (H, Part, Crc32, &Size, &Crc) && Size == S && Crc == Img, "good header");
    for (int I = 0; I < 32; I++) {
      UINT8 B[4096];
      memcpy (B, H, sizeof B);
      B[I] ^= 0x10;
      CHECK (!HandbackCheckHeader (B, Part, Crc32, &Size, &Crc), "header byte %d", I);
    }
    CHECK (!HandbackCheckHeader (H, (1ull << 20) + 4096 + S - 1, Crc32, &Size, &Crc), "past the partition");
    CHECK (!HandbackCheckHeader (H, 1000, Crc32, &Size, &Crc), "a partition smaller than the offset");
    UINT8 Z[4096] = {0};
    CHECK (!HandbackCheckHeader (Z, Part, Crc32, &Size, &Crc), "zeros");
    /* Too large, with a good CRC. */
    S = (3ull << 20) - 4096 + 1;
    for (int I = 0; I < 8; I++) H[16 + I] = (UINT8)(S >> (8 * I));
    Hc = Crc32 (H, 28);
    for (int I = 0; I < 4; I++) H[28 + I] = (UINT8)(Hc >> (8 * I));
    CHECK (!HandbackCheckHeader (H, 64ull << 20, Crc32, &Size, &Crc), "too large");
  }

  printf ("%s: %d failures\n", Fails ? "FAIL" : "ok", Fails);
  return Fails != 0;
}
