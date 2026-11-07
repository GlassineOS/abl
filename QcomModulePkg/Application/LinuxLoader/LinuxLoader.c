/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of The Linux Foundation nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "AutoGen.h"
#include "BootLinux.h"
#include "BootStats.h"
#include "KeyPad.h"
#include "LinuxLoaderLib.h"
#include <FastbootLib/FastbootMain.h>
#include <Library/DeviceInfo.h>
#include <Library/DrawUI.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/ShutdownServices.h>
#include <Library/StackCanary.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/DxeServicesTableLib.h>

#define MAX_APP_STR_LEN 64
#define MAX_NUM_FS 10
#define DEFAULT_STACK_CHK_GUARD 0xc0c0c0c0

/* NA1: Hall sensor GPIO 105 — south tile address.
 * Flat range (0x03069xxx) is XPU-blocked from ABL; south tile works.
 * Measured with `fastboot oem test-sensor`: south reads, flat hangs. */
#define NA1_HALL_GPIO105_PAGE  0x03169000ULL
#define NA1_HALL_GPIO_INOUT    0x03169004

STATIC BOOLEAN BootReasonAlarm = FALSE;
STATIC BOOLEAN BootIntoFastboot = FALSE;
STATIC BOOLEAN BootIntoRecovery = FALSE;

STATIC VOID* UnSafeStackPtr;

STATIC EFI_STATUS __attribute__ ( (no_sanitize ("safe-stack")))
AllocateUnSafeStackPtr (VOID)
{

  EFI_STATUS Status = EFI_SUCCESS;

  UnSafeStackPtr = AllocatePool (BOOT_LOADER_MAX_UNSAFE_STACK_SIZE);
  if (UnSafeStackPtr == NULL) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate memory for UnSafeStack \n"));
    Status = EFI_OUT_OF_RESOURCES;
    return Status;
  }

  UnSafeStackPtr += BOOT_LOADER_MAX_UNSAFE_STACK_SIZE;

  return Status;
}

//This function is to return the Unsafestack ptr address
VOID** __attribute__ ( (no_sanitize ("safe-stack")))
__safestack_pointer_address (VOID)
{

  return (VOID**) &UnSafeStackPtr;
}

// This function is used to Deactivate MDTP by entering recovery UI

STATIC EFI_STATUS MdtpDisable (VOID)
{
  BOOLEAN MdtpActive = FALSE;
  EFI_STATUS Status = EFI_SUCCESS;
  QCOM_MDTP_PROTOCOL *MdtpProtocol;

  if (FixedPcdGetBool (EnableMdtpSupport)) {
    Status = IsMdtpActive (&MdtpActive);

    if (EFI_ERROR (Status))
      return Status;

    if (MdtpActive) {
      Status = gBS->LocateProtocol (&gQcomMdtpProtocolGuid, NULL,
                                    (VOID **)&MdtpProtocol);
      if (EFI_ERROR (Status)) {
        DEBUG ((EFI_D_ERROR, "Failed to locate MDTP protocol, Status=%r\n",
                Status));
        return Status;
      }
      /* Perform Local Deactivation of MDTP */
      Status = MdtpProtocol->MdtpDeactivate (MdtpProtocol, FALSE);
    }
  }

  return Status;
}

STATIC UINT8
GetRebootReason (UINT32 *ResetReason)
{
  EFI_RESETREASON_PROTOCOL *RstReasonIf;
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&gEfiResetReasonProtocolGuid, NULL,
                                (VOID **)&RstReasonIf);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error locating the reset reason protocol\n"));
    return Status;
  }

  RstReasonIf->GetResetReason (RstReasonIf, ResetReason, NULL, NULL);
  if (RstReasonIf->Revision >= EFI_RESETREASON_PROTOCOL_REVISION)
    RstReasonIf->ClearResetReason (RstReasonIf);
  return Status;
}

/**
  Linux Loader Application EntryPoint

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.

 **/

EFI_STATUS EFIAPI  __attribute__ ( (no_sanitize ("safe-stack")))
LinuxLoaderEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  UINT32 BootReason = NORMAL_MODE;
  UINT32 KeyPressed;
  /* MultiSlot Boot */
  BOOLEAN MultiSlotBoot;

  DEBUG ((EFI_D_INFO, "Loader Build Info: %a %a\n", __DATE__, __TIME__));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoader Load Address to debug ABL: 0x%llx\n",
         (UINTN)LinuxLoaderEntry & (~ (0xFFF))));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoaderEntry Address: 0x%llx\n",
         (UINTN)LinuxLoaderEntry));

  Status = AllocateUnSafeStackPtr ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Unable to Allocate memory for Unsafe Stack: %r\n",
            Status));
    goto stack_guard_update_default;
  }

  StackGuardChkSetup ();

  BootStatsSetTimeStamp (BS_BL_START);

  // Initialize verified boot & Read Device Info
  Status = DeviceInfoInit ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Initialize the device info failed: %r\n", Status));
    goto stack_guard_update_default;
  }

  Status = EnumeratePartitions ();

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LinuxLoader: Could not enumerate partitions: %r\n",
            Status));
    goto stack_guard_update_default;
  }

  UpdatePartitionEntries ();
  /*Check for multislot boot support*/
  MultiSlotBoot = PartitionHasMultiSlot ((CONST CHAR16 *)L"boot");
  if (MultiSlotBoot) {
    DEBUG ((EFI_D_VERBOSE, "Multi Slot boot is supported\n"));
    FindPtnActiveSlot ();
  }

  Status = GetKeyPress (&KeyPressed);
  if (Status == EFI_SUCCESS) {
    if (KeyPressed == SCAN_DOWN)
      BootIntoFastboot = TRUE;
    if (KeyPressed == SCAN_UP)
      BootIntoRecovery = TRUE;
    if (KeyPressed == SCAN_ESC)
      RebootDevice (EMERGENCY_DLOAD);
  } else if (Status == EFI_DEVICE_ERROR) {
    DEBUG ((EFI_D_ERROR, "Error reading key status: %r\n", Status));
    goto stack_guard_update_default;
  }

  /* NA1: Hall sensor — magnetic cover as fastboot trigger.
   * Close the cover during power-on → hold 3s → enters fastboot mode.
   * Replaces vol-up key (device has no volume buttons).
   * Uses south tile address — flat range is XPU-blocked in ABL. */
  if (!BootIntoFastboot && !BootIntoRecovery) {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR Na1GcdDesc;

    /* Ensure TLMM page is GCD-mapped */
    if (EFI_ERROR (gDS->GetMemorySpaceDescriptor (
          NA1_HALL_GPIO105_PAGE, &Na1GcdDesc)) ||
        Na1GcdDesc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo,
                           NA1_HALL_GPIO105_PAGE, 0x1000, 0);
      gDS->SetMemorySpaceAttributes (
        NA1_HALL_GPIO105_PAGE, 0x1000, EFI_MEMORY_UC);
    }

    {
      UINT32 HallVal = MmioRead32 (NA1_HALL_GPIO_INOUT);
      if (!(HallVal & 0x1)) {
        /* Cover closed — wait 3s and re-check (debounce) */
        DEBUG ((EFI_D_INFO, "NA1: Cover closed at boot, confirming (3s)...\n"));
        MicroSecondDelay (3000000);
        HallVal = MmioRead32 (NA1_HALL_GPIO_INOUT);
        if (!(HallVal & 0x1)) {
          DEBUG ((EFI_D_INFO, "NA1: Cover confirmed — entering fastboot\n"));
          BootIntoFastboot = TRUE;
        }
      }
    }
  }

  // check for reboot mode
  Status = GetRebootReason (&BootReason);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Failed to get Reboot reason: %r\n", Status));
    goto stack_guard_update_default;
  }

  switch (BootReason) {
  case FASTBOOT_MODE:
    BootIntoFastboot = TRUE;
    break;
  case RECOVERY_MODE:
    BootIntoRecovery = TRUE;
    break;
  case ALARM_BOOT:
    BootReasonAlarm = TRUE;
    break;
  case DM_VERITY_ENFORCING:
    // write to device info
    Status = EnableEnforcingMode (TRUE);
    if (Status != EFI_SUCCESS)
      goto stack_guard_update_default;
    break;
  case DM_VERITY_LOGGING:
    /* Disable MDTP if it's Enabled through Local Deactivation */
    Status = MdtpDisable ();
    if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) {
      DEBUG ((EFI_D_ERROR, "MdtpDisable Returned error: %r\n", Status));
      goto stack_guard_update_default;
    }
    // write to device info
    Status = EnableEnforcingMode (FALSE);
    if (Status != EFI_SUCCESS)
      goto stack_guard_update_default;

    break;
  case DM_VERITY_KEYSCLEAR:
    Status = ResetDeviceState ();
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "VB Reset Device State error: %r\n", Status));
      goto stack_guard_update_default;
    }
    break;
  default:
    if (BootReason != NORMAL_MODE) {
      DEBUG ((EFI_D_ERROR,
             "Boot reason: 0x%x not handled, defaulting to Normal Boot\n",
             BootReason));
    }
    break;
  }

  Status = RecoveryInit (&BootIntoRecovery);
  if (Status != EFI_SUCCESS)
    DEBUG ((EFI_D_VERBOSE, "RecoveryInit failed ignore: %r\n", Status));

  /* Populate board data required for fastboot, dtb selection and cmd line */
  Status = BoardInit ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error finding board information: %r\n", Status));
    return Status;
  }

  if (!BootIntoFastboot) {
    BootInfo Info = {0};
    Info.MultiSlotBoot = MultiSlotBoot;
    Info.BootIntoRecovery = BootIntoRecovery;
    Info.BootReasonAlarm = BootReasonAlarm;
    Status = LoadImageAndAuth (&Info);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "LoadImageAndAuth failed: %r\n", Status));
      goto fastboot;
    }

    /* NA1: Set Root of Trust via AVB_1 protocol (VBSendRot).
     *
     * With VERIFIED_BOOT=0 (NO_AVB), LoadImageAndAuth skips the AVB path
     * entirely. The FDE encryption key is bound to the RoT that the stock
     * ABL (AVB_1) set via VBSendRot. We must use the same protocol — NOT
     * KeyMasterSetRotAndBootState (AVB_2 API) which produces a different
     * RoT digest and causes FDE key mismatch.
     *
     * Flow: LocateProtocol → VBDeviceInit (unlocked) → VBVerifyImage
     *       (sets BootState=ORANGE on unlocked device) → VBSendRot. */
    {
      QCOM_VERIFIEDBOOT_PROTOCOL *VbIntf = NULL;
      device_info_vb_t DevInfo_vb;
      CHAR8 PnameAscii[MAX_GPT_NAME_SIZE];
      CHAR8 StrPname[MAX_GPT_NAME_SIZE];

      Status = gBS->LocateProtocol (&gEfiQcomVerifiedBootProtocolGuid, NULL,
                                    (VOID **)&VbIntf);
      if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "NA1: Cannot locate VB protocol: %r\n", Status));
        goto na1_vb_done;
      }

      DevInfo_vb.is_unlocked = IsUnlocked ();
      DevInfo_vb.is_unlock_critical = IsUnlockCritical ();
      Status = VbIntf->VBDeviceInit (VbIntf, &DevInfo_vb);
      if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "NA1: VBDeviceInit failed: %r\n", Status));
        goto na1_vb_done;
      }

      /* Build partition name and select image data for VBVerifyImage.
       *
       * VBSendRot sends a RoT digest that TZ uses to seal/unseal the ICE
       * DKMK (Disk Key Master Key).  The digest is derived from the image
       * data passed to VBVerifyImage.  If we pass recovery.img data during
       * a recovery boot, TZ records a different RoT than the one used when
       * Android sealed the DKMK → set_hw_device_encryption_key returns -7.
       *
       * Fix: when booting recovery, load boot.img so VBVerifyImage sees
       * the same image data as during a normal Android boot → same RoT →
       * TZ can unseal the DKMK.
       *
       * Boot partition is 64 MB on NA1.  Allocate a fixed 64 MB buffer,
       * read boot.img, pass it to VBVerifyImage, free after VBSendRot.
       * On any failure, fall back to recovery.img data — TWRP still boots
       * even if FDE decrypt fails. */
      VOID  *VbImgBuf  = Info.Images[0].ImageBuffer;
      UINT32 VbImgSize = Info.Images[0].ImageSize;
      VOID  *BootBuf   = NULL;

      if (BootIntoRecovery) {
        STATIC CONST UINT32 BootPartSz = 64 * 1024 * 1024; /* 64 MB */
        UINT32 BootSz = BootPartSz;
        CHAR16 BootPname[MAX_GPT_NAME_SIZE];
        CHAR8  BootRotName[MAX_GPT_NAME_SIZE];

        /* NA1: select boot_<activeslot> on an A/B GPT, plain "boot" on a
         * single-slot GPT.  The RoT digest TZ records from this image must
         * match the boot image of the slot Android sealed the DKMK under, or
         * set_hw_device_encryption_key returns -7 (FDE will not decrypt).  On
         * an A/B GPT there is no unsuffixed `boot` partition, so loading
         * L"boot" would fail → recovery.img fallback → wrong RoT → the
         * FDE data cannot be read from recovery.  The partition-name string
         * (BootRotName) is kept in lock-step so it matches what a normal boot
         * of the active slot passes to VBVerifyImage. */
        StrnCpyS (BootPname, ARRAY_SIZE (BootPname), L"boot", StrLen (L"boot"));
        AsciiStrnCpyS (BootRotName, ARRAY_SIZE (BootRotName), "/boot",
                       AsciiStrLen ("/boot"));
        if (MultiSlotBoot) {
          Slot CurSlot = GetCurrentSlotSuffix ();
          CHAR8 SuffixAscii[MAX_SLOT_SUFFIX_SZ] = {0};

          StrnCatS (BootPname, ARRAY_SIZE (BootPname), CurSlot.Suffix,
                    StrLen (CurSlot.Suffix));
          UnicodeStrToAsciiStr (CurSlot.Suffix, SuffixAscii);
          AsciiStrnCatS (BootRotName, ARRAY_SIZE (BootRotName), SuffixAscii,
                         AsciiStrLen (SuffixAscii));
        }

        BootBuf = AllocatePages (ALIGN_PAGES (BootPartSz, ALIGNMENT_MASK_4KB));
        if (BootBuf) {
          EFI_STATUS Rs = LoadImageFromPartition (BootBuf, &BootSz, BootPname);
          if (Rs == EFI_SUCCESS) {
            VbImgBuf  = BootBuf;
            VbImgSize = BootSz;
            AsciiStrnCpyS (StrPname, ARRAY_SIZE (StrPname), BootRotName,
                           AsciiStrLen (BootRotName));
            DEBUG ((EFI_D_INFO,
                    "NA1: recovery boot — loaded %s for RoT (%u bytes)\n",
                    BootPname, BootSz));
          } else {
            /* Fall back: use recovery.img, FDE decrypt will likely fail. */
            FreePages (BootBuf,
                       ALIGN_PAGES (BootPartSz, ALIGNMENT_MASK_4KB));
            BootBuf = NULL;
            DEBUG ((EFI_D_WARN,
                    "NA1: boot load for RoT failed %r — using recovery, "
                    "FDE may not decrypt\n", Rs));
            AsciiStrnCpyS (StrPname, ARRAY_SIZE (StrPname), "/recovery",
                           AsciiStrLen ("/recovery"));
          }
        } else {
          DEBUG ((EFI_D_WARN,
                  "NA1: no memory for boot RoT buf — FDE may not decrypt\n"));
          AsciiStrnCpyS (StrPname, ARRAY_SIZE (StrPname), "/recovery",
                         AsciiStrLen ("/recovery"));
        }
      } else {
        /* Normal boot: Info.Pname = L"boot", produce "/boot". */
        AsciiStrnCpyS (StrPname, ARRAY_SIZE (StrPname), "/",
                       AsciiStrLen ("/"));
        UnicodeStrToAsciiStr (Info.Pname, PnameAscii);
        AsciiStrnCatS (StrPname, ARRAY_SIZE (StrPname), PnameAscii,
                       AsciiStrLen (PnameAscii));
      }

      Status = VbIntf->VBVerifyImage (VbIntf, (UINT8 *)StrPname,
                                      (UINT8 *)VbImgBuf, VbImgSize,
                                      &Info.BootState);
      if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "NA1: VBVerifyImage failed: %r (unlocked, "
                "continuing as ORANGE)\n", Status));
        Info.BootState = ORANGE;
        /* On unlocked device, verification failure is expected with
         * a patched boot image. Continue — VBSendRot just needs
         * DeviceInit state, not verification success. */
      }

      Status = VbIntf->VBSendRot (VbIntf);
      if (BootBuf) {
        FreePages (BootBuf, ALIGN_PAGES (64 * 1024 * 1024, ALIGNMENT_MASK_4KB));
        BootBuf = NULL;
      }
      if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "NA1: VBSendRot failed: %r\n", Status));
      } else {
        DEBUG ((EFI_D_INFO, "NA1: VBSendRot success, BootState=%d\n",
                Info.BootState));
      }

na1_vb_done:
      if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "NA1: VB RoT setup failed — FDE may not "
                "decrypt. Continuing to boot.\n"));
      }
    }

    /* NA1: Override VBCmdLine with stock-matching parameters.
     *
     * The NO_AVB path (LoadImageNoAuthWrapper) set VBCmdLine to a bare
     * "root=/dev/mmcblk0pN" via GetSystemPath. The stock ABL (AVB_1) instead
     * produced dm-verity params + verified boot state. We overwrite the
     * buffer to replicate the stock cmdline exactly.
     *
     * Buffer was allocated at DTB_PAD_SIZE (1024) by VBAllocateCmdLine. */
    if (Info.VBCmdLine != NULL) {
      STATIC CONST CHAR8 *NA1VBCmdLine =
          " veritykeyid=id:7e4333f9bba00adfe0ede979e28ed1920492b40f"
          " root=/dev/dm-0"
          " dm=\"system none ro,0 1 android-verity /dev/mmcblk0p10\""
          " androidboot.verifiedbootstate=orange"
          " androidboot.keymaster=1"
          "  androidboot.veritymode=enforcing";

      AsciiStrCpyS (Info.VBCmdLine, Info.VBCmdLineLen, NA1VBCmdLine);
      Info.VBCmdLineFilledLen = AsciiStrLen (NA1VBCmdLine);

      /* NA1: make the dm-verity backing device track the active slot.
       * The literal above names system_a (mmcblk0p10); on slot _b the backing
       * is system_b (mmcblk0p50).  Look up the current slot's system index and
       * patch the two digits in place.  Both system_a (10) and system_b (50)
       * are two-digit indices, so this is byte-for-byte size-neutral in the
       * cmdline buffer and needs no PrintLib.  Without this BOTH slots booted
       * system_a (p10), so a restore to the inactive slot's system was never
       * read. */
      if (MultiSlotBoot) {
        Slot   CurSlot = GetCurrentSlotSuffix ();
        CHAR16 SysPname[MAX_GPT_NAME_SIZE];
        INT32  Idx;

        StrnCpyS (SysPname, ARRAY_SIZE (SysPname), L"system",
                  StrLen (L"system"));
        StrnCatS (SysPname, ARRAY_SIZE (SysPname), CurSlot.Suffix,
                  StrLen (CurSlot.Suffix));
        Idx = GetPartitionIndex (SysPname);
        if (Idx >= 10 && Idx < 100) {
          CHAR8 *P = AsciiStrStr (Info.VBCmdLine, "mmcblk0p10");
          if (P != NULL) {
            P[8] = (CHAR8)('0' + (Idx / 10));
            P[9] = (CHAR8)('0' + (Idx % 10));
          }
        }
      }

      DEBUG ((EFI_D_INFO, "NA1: VBCmdLine overridden, len=%d\n",
              Info.VBCmdLineFilledLen));
    }

    BootLinux (&Info);
  }

fastboot:
  DEBUG ((EFI_D_INFO, "Launching fastboot\n"));
  Status = FastbootInitialize ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Launch Fastboot App: %d\n", Status));
    goto stack_guard_update_default;
  }

stack_guard_update_default:
  /*Update stack check guard with defualt value then return*/
  __stack_chk_guard = DEFAULT_STACK_CHK_GUARD;
  return Status;
}
