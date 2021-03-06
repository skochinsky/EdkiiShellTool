/** @file

Copyright (c) 2018, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials 
are licensed and made available under the terms and conditions of the BSD License 
which accompanies this distribution.  The full text of the license may be found at 
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS, 
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <PiDxe.h>

#include <Protocol/Tcg2Protocol.h>

#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/Tpm2CommandLib.h>
#include <Library/ShellLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseCryptLib.h>

#define EV_NO_ACTION                ((TCG_EVENTTYPE) 0x00000003)

typedef struct {
  EFI_TCG2_EVENT_LOG_FORMAT  LogFormat;
} EFI_TCG2_EVENT_INFO_STRUCT;

EFI_TCG2_EVENT_INFO_STRUCT mTcg2EventInfo[] = {
  {EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2},
  {EFI_TCG2_EVENT_LOG_FORMAT_TCG_2},
};

typedef
UINTN
(EFIAPI *EFI_HASH_GET_CONTEXT_SIZE) (
  VOID
  );

typedef
BOOLEAN
(EFIAPI *EFI_HASH_INIT) (
  OUT  VOID  *HashContext
  );

typedef
BOOLEAN
(EFIAPI *EFI_HASH_UPDATE) (
  IN OUT  VOID        *HashContext,
  IN      CONST VOID  *Data,
  IN      UINTN       DataSize
  );

typedef
BOOLEAN
(EFIAPI *EFI_HASH_FINAL) (
  IN OUT  VOID   *HashContext,
  OUT     UINT8  *HashValue
  );

typedef struct {
  TPM_ALG_ID                 HashAlg;
  EFI_HASH_GET_CONTEXT_SIZE  GetContextSize;
  EFI_HASH_INIT              Init;
  EFI_HASH_UPDATE            Update;
  EFI_HASH_FINAL             Final;
} EFI_HASH_INFO;


EFI_HASH_INFO  mHashInfo[] = {
  {TPM_ALG_SHA1,   Sha1GetContextSize,   Sha1Init,   Sha1Update,   Sha1Final,   },
  {TPM_ALG_SHA256, Sha256GetContextSize, Sha256Init, Sha256Update, Sha256Final, },
};

#define PCR_INDEX_ALL 0xFFFFFFFF

SHELL_PARAM_ITEM mParamList[] = {
  {L"-I",   TypeValue},
  {L"-L",   TypeValue},
  {L"-E",   TypeFlag},
  {L"-C",   TypeFlag},
  {L"-?",   TypeFlag},
  {L"-h",   TypeFlag},
  {NULL,    TypeMax},
  };

/**

  This function dump raw data.

  @param  Data  raw data
  @param  Size  raw data size

**/
VOID
InternalDumpData (
  IN UINT8  *Data,
  IN UINTN  Size
  )
{
  UINTN  Index;
  for (Index = 0; Index < Size; Index++) {
    Print (L"%02x", (UINTN)Data[Index]);
  }
}

/**

  This function dump raw data with colume format.

  @param  Data  raw data
  @param  Size  raw data size

**/
VOID
InternalDumpHex (
  IN UINT8  *Data,
  IN UINTN  Size
  )
{
  UINTN   Index;
  UINTN   Count;
  UINTN   Left;

#define COLUME_SIZE  (16 * 2)

  Count = Size / COLUME_SIZE;
  Left  = Size % COLUME_SIZE;
  for (Index = 0; Index < Count; Index++) {
    Print (L"%04x: ", Index * COLUME_SIZE);
    InternalDumpData (Data + Index * COLUME_SIZE, COLUME_SIZE);
    Print (L"\n");
  }

  if (Left != 0) {
    Print (L"%04x: ", Index * COLUME_SIZE);
    InternalDumpData (Data + Index * COLUME_SIZE, Left);
    Print (L"\n");
  }
}

/**
  Dump PCR data.

  @param PcrIndex Pcr index
  @param HashAlgo Hash algorithm
**/
VOID
DumpPcr (
  IN TPMI_DH_PCR               PcrIndex,
  IN TPM_ALG_ID                HashAlgo
  )
{
  EFI_STATUS                Status;
  TPML_PCR_SELECTION        PcrSelectionIn;
  UINT32                    PcrUpdateCounter;
  TPML_PCR_SELECTION        PcrSelectionOut;
  TPML_DIGEST               PcrValues;
  UINTN                     Index;

  ZeroMem (&PcrSelectionIn, sizeof(PcrSelectionIn));
  PcrUpdateCounter = 0;
  ZeroMem (&PcrSelectionOut, sizeof(PcrSelectionOut));
  ZeroMem (&PcrValues, sizeof(PcrValues));

  //
  // Fill input
  //
  PcrSelectionIn.count = 1;
  PcrSelectionIn.pcrSelections[0].hash = HashAlgo;
  PcrSelectionIn.pcrSelections[0].sizeofSelect = PCR_SELECT_MAX;
  PcrSelectionIn.pcrSelections[0].pcrSelect[PcrIndex / 8] = (1 << (PcrIndex % 8));
  Status = Tpm2PcrRead (&PcrSelectionIn, &PcrUpdateCounter, &PcrSelectionOut, &PcrValues);
  if (EFI_ERROR (Status)) {
    Print (L"Tpm2PcrRead - %r\n", Status);
    return ;
  }

  //
  // DumpPcr
  //
  for (Index = 0; Index < PcrValues.count; Index++) {
    Print (L"PCR[%d] (Hash:0x%x): ", PcrIndex, HashAlgo);
    InternalDumpData ((UINT8 *)&PcrValues.digests[Index].buffer, PcrValues.digests[Index].size);
    Print (L"\n");
  }
}

EFI_HASH_INFO *
GetHashInfo (
  IN     TPM_ALG_ID  HashAlg
  )
{
  UINTN      Index;

  for (Index = 0; Index < sizeof(mHashInfo)/sizeof(mHashInfo[0]); Index++) {
    if (HashAlg == mHashInfo[Index].HashAlg) {
      return &mHashInfo[Index];
    }
  }
  return NULL;
}

VOID
ExtendEvent (
  IN     TPM_ALG_ID  HashAlg,
  IN OUT VOID        *TcgDigest,
  IN     VOID        *NewDigest
  )
{
  VOID                     *HashCtx;
  UINTN                    CtxSize;
  UINT16                   DigestSize;
  EFI_HASH_INFO            *HashInfo;

  DigestSize = GetHashSizeFromAlgo (HashAlg);

  HashInfo = GetHashInfo (HashAlg);
  if (HashInfo == NULL) {
    SetMem (TcgDigest, DigestSize, 0xFF);
    return ;
  }

  CtxSize = HashInfo->GetContextSize ();
  HashCtx = AllocatePool (CtxSize);
  if (HashCtx == NULL) {
    SetMem (TcgDigest, DigestSize, 0xFF);
    return ;
  }
  HashInfo->Init (HashCtx);
  HashInfo->Update (HashCtx, TcgDigest, DigestSize);
  HashInfo->Update (HashCtx, NewDigest, DigestSize);
  HashInfo->Final (HashCtx, (UINT8 *)TcgDigest);
  FreePool (HashCtx);
}

VOID
DumpEvent (
  IN TCG_PCR_EVENT_HDR         *EventHdr
  )
{
  UINTN                     Index;

  Print (L"  Event:\n");
  Print (L"    PCRIndex  - %d\n", EventHdr->PCRIndex);
  Print (L"    EventType - 0x%08x\n", EventHdr->EventType);
  Print (L"    Digest    - ");
  for (Index = 0; Index < sizeof(TCG_DIGEST); Index++) {
    Print (L"%02x", EventHdr->Digest.digest[Index]);
  }
  Print (L"\n");
  Print (L"    EventSize - 0x%08x\n", EventHdr->EventSize);
  InternalDumpHex ((UINT8 *)(EventHdr + 1), EventHdr->EventSize);
}

/**
  This function dump TCG_EfiSpecIDEventStruct.

  @param[in]  TcgEfiSpecIdEventStruct     A pointer to TCG_EfiSpecIDEventStruct.
**/
VOID
DumpTcgEfiSpecIdEventStruct (
  IN TCG_EfiSpecIDEventStruct   *TcgEfiSpecIdEventStruct
  )
{
  TCG_EfiSpecIdEventAlgorithmSize  *digestSize;
  UINTN                            Index;
  UINT8                            *vendorInfoSize;
  UINT8                            *vendorInfo;
  UINT32                           numberOfAlgorithms;

  Print (L"  TCG_EfiSpecIDEventStruct:\n");
  Print (L"    signature          - '");
  for (Index = 0; Index < sizeof(TcgEfiSpecIdEventStruct->signature); Index++) {
    Print (L"%c", TcgEfiSpecIdEventStruct->signature[Index]);
  }
  Print (L"'\n");
  Print (L"    platformClass      - 0x%08x\n", TcgEfiSpecIdEventStruct->platformClass);
  Print (L"    specVersion        - %d.%d%d\n", TcgEfiSpecIdEventStruct->specVersionMajor, TcgEfiSpecIdEventStruct->specVersionMinor, TcgEfiSpecIdEventStruct->specErrata);
  Print (L"    uintnSize          - 0x%02x\n", TcgEfiSpecIdEventStruct->uintnSize);

  CopyMem (&numberOfAlgorithms, TcgEfiSpecIdEventStruct + 1, sizeof(numberOfAlgorithms));
  Print (L"    numberOfAlgorithms - 0x%08x\n", numberOfAlgorithms);

  digestSize = (TCG_EfiSpecIdEventAlgorithmSize *)((UINT8 *)TcgEfiSpecIdEventStruct + sizeof(*TcgEfiSpecIdEventStruct) + sizeof(numberOfAlgorithms));
  for (Index = 0; Index < numberOfAlgorithms; Index++) {
    Print (L"    digest(%d)\n", Index);
    Print (L"      algorithmId      - 0x%04x\n", digestSize[Index].algorithmId);
    Print (L"      digestSize       - 0x%04x\n", digestSize[Index].digestSize);
  }
  vendorInfoSize = (UINT8 *)&digestSize[numberOfAlgorithms];
  Print (L"    vendorInfoSize     - 0x%02x\n", *vendorInfoSize);
  vendorInfo = vendorInfoSize + 1;
  Print (L"    vendorInfo         - ");
  for (Index = 0; Index < *vendorInfoSize; Index++) {
    Print (L"%02x", vendorInfo[Index]);
  }
  Print (L"\n");
}

/**
  This function get size of TCG_EfiSpecIDEventStruct.

  @param[in]  TcgEfiSpecIdEventStruct     A pointer to TCG_EfiSpecIDEventStruct.
**/
UINTN
GetTcgEfiSpecIdEventStructSize (
  IN TCG_EfiSpecIDEventStruct   *TcgEfiSpecIdEventStruct
  )
{
  TCG_EfiSpecIdEventAlgorithmSize  *digestSize;
  UINT8                            *vendorInfoSize;
  UINT32                           numberOfAlgorithms;

  CopyMem (&numberOfAlgorithms, TcgEfiSpecIdEventStruct + 1, sizeof(numberOfAlgorithms));

  digestSize = (TCG_EfiSpecIdEventAlgorithmSize *)((UINT8 *)TcgEfiSpecIdEventStruct + sizeof(*TcgEfiSpecIdEventStruct) + sizeof(numberOfAlgorithms));
  vendorInfoSize = (UINT8 *)&digestSize[numberOfAlgorithms];
  return sizeof(TCG_EfiSpecIDEventStruct) + sizeof(UINT32) + (numberOfAlgorithms * sizeof(TCG_EfiSpecIdEventAlgorithmSize)) + sizeof(UINT8) + (*vendorInfoSize);
}

VOID
DumpEvent2 (
  IN TCG_PCR_EVENT2        *TcgPcrEvent2
  )
{
  UINTN                     Index;
  UINT32                    DigestIndex;
  UINT32                    DigestCount;
  TPMI_ALG_HASH             HashAlgo;
  UINT32                    DigestSize;
  UINT8                     *DigestBuffer;
  UINT32                    EventSize;
  UINT8                     *EventBuffer;

  Print (L"  Event:\n");
  Print (L"    PCRIndex  - %d\n", TcgPcrEvent2->PCRIndex);
  Print (L"    EventType - 0x%08x\n", TcgPcrEvent2->EventType);

  Print (L"    DigestCount: 0x%08x\n", TcgPcrEvent2->Digest.count);

  DigestCount = TcgPcrEvent2->Digest.count;
  HashAlgo = TcgPcrEvent2->Digest.digests[0].hashAlg;
  DigestBuffer = (UINT8 *)&TcgPcrEvent2->Digest.digests[0].digest;
  for (DigestIndex = 0; DigestIndex < DigestCount; DigestIndex++) {
    Print (L"    HashAlgo : 0x%04x\n", HashAlgo);
    Print (L"    Digest(%d): ", DigestIndex);
    DigestSize = GetHashSizeFromAlgo (HashAlgo);
    for (Index = 0; Index < DigestSize; Index++) {
      Print (L"%02x", DigestBuffer[Index]);
    }
    Print (L"\n");
    //
    // Prepare next
    //
    CopyMem (&HashAlgo, DigestBuffer + DigestSize, sizeof(TPMI_ALG_HASH));
    DigestBuffer = DigestBuffer + DigestSize + sizeof(TPMI_ALG_HASH);
  }
  DigestBuffer = DigestBuffer - sizeof(TPMI_ALG_HASH);

  CopyMem (&EventSize, DigestBuffer, sizeof(TcgPcrEvent2->EventSize));
  Print (L"    EventSize - 0x%08x\n", EventSize);
  EventBuffer = DigestBuffer + sizeof(TcgPcrEvent2->EventSize);
  InternalDumpHex (EventBuffer, EventSize);
}

UINTN
GetPcrEvent2Size (
  IN TCG_PCR_EVENT2        *TcgPcrEvent2
  )
{
  UINT32                    DigestIndex;
  UINT32                    DigestCount;
  TPMI_ALG_HASH             HashAlgo;
  UINT32                    DigestSize;
  UINT8                     *DigestBuffer;
  UINT32                    EventSize;
  UINT8                     *EventBuffer;

  DigestCount = TcgPcrEvent2->Digest.count;
  HashAlgo = TcgPcrEvent2->Digest.digests[0].hashAlg;
  DigestBuffer = (UINT8 *)&TcgPcrEvent2->Digest.digests[0].digest;
  for (DigestIndex = 0; DigestIndex < DigestCount; DigestIndex++) {
    DigestSize = GetHashSizeFromAlgo (HashAlgo);
    //
    // Prepare next
    //
    CopyMem (&HashAlgo, DigestBuffer + DigestSize, sizeof(TPMI_ALG_HASH));
    DigestBuffer = DigestBuffer + DigestSize + sizeof(TPMI_ALG_HASH);
  }
  DigestBuffer = DigestBuffer - sizeof(TPMI_ALG_HASH);

  CopyMem (&EventSize, DigestBuffer, sizeof(TcgPcrEvent2->EventSize));
  EventBuffer = DigestBuffer + sizeof(TcgPcrEvent2->EventSize);

  return (UINTN)EventBuffer + EventSize - (UINTN)TcgPcrEvent2;
}

UINT8 *
GetDigestFromPcrEvent2 (
  IN TCG_PCR_EVENT2            *TcgPcrEvent2,
  IN TPMI_ALG_HASH             HashAlg
  )
{
  UINT32                    DigestIndex;
  UINT32                    DigestCount;
  TPMI_ALG_HASH             HashAlgo;
  UINT32                    DigestSize;
  UINT8                     *DigestBuffer;

  DigestCount = TcgPcrEvent2->Digest.count;
  HashAlgo = TcgPcrEvent2->Digest.digests[0].hashAlg;
  DigestBuffer = (UINT8 *)&TcgPcrEvent2->Digest.digests[0].digest;
  for (DigestIndex = 0; DigestIndex < DigestCount; DigestIndex++) {
    DigestSize = GetHashSizeFromAlgo (HashAlgo);

    if (HashAlg == HashAlgo) {
      return DigestBuffer;
    }

    //
    // Prepare next
    //
    CopyMem (&HashAlgo, DigestBuffer + DigestSize, sizeof(TPMI_ALG_HASH));
    DigestBuffer = DigestBuffer + DigestSize + sizeof(TPMI_ALG_HASH);
  }
  return NULL;
}

UINT32
GetTcgSpecIdNumberOfAlgorithms (
  IN TCG_EfiSpecIDEventStruct *TcgEfiSpecIdEventStruct
  )
{
  UINT32                           numberOfAlgorithms;

  CopyMem (&numberOfAlgorithms, TcgEfiSpecIdEventStruct + 1, sizeof(numberOfAlgorithms));
  return numberOfAlgorithms;
}

TCG_EfiSpecIdEventAlgorithmSize *
GetTcgSpecIdDigestSize (
  IN TCG_EfiSpecIDEventStruct *TcgEfiSpecIdEventStruct
  )
{
  return (TCG_EfiSpecIdEventAlgorithmSize *)((UINT8 *)TcgEfiSpecIdEventStruct + sizeof(*TcgEfiSpecIdEventStruct) + sizeof(UINT32));
}

/**
  This function dump event log.

  @param[in]  EventLogFormat     The type of the event log for which the information is requested.
  @param[in]  EventLogLocation   A pointer to the memory address of the event log.
  @param[in]  EventLogLastEntry  If the Event Log contains more than one entry, this is a pointer to the
                                 address of the start of the last entry in the event log in memory.
  @param[in]  FinalEventsTable   A pointer to the memory address of the final event table.
**/
VOID
DumpEventLog (
  IN EFI_TCG2_EVENT_LOG_FORMAT   EventLogFormat,
  IN EFI_PHYSICAL_ADDRESS        EventLogLocation,
  IN EFI_PHYSICAL_ADDRESS        EventLogLastEntry,
  IN EFI_TCG2_FINAL_EVENTS_TABLE *FinalEventsTable,
  IN UINT32                      PcrIndex,
  IN BOOLEAN                     CalculateExpected
  )
{
  TCG_PCR_EVENT_HDR                *EventHdr;
  UINTN                            Index;
  TCG_DIGEST                       TcgDigest;
  TCG_PCR_EVENT2                   *TcgPcrEvent2;
  TCG_EfiSpecIDEventStruct         *TcgEfiSpecIdEventStruct;
  UINT32                           numberOfAlgorithms;
  TCG_EfiSpecIdEventAlgorithmSize  *digestSize;
  UINT8                            *DigestBuffer;
  TPMI_ALG_HASH                    HashAlg;
  UINTN                            NumberOfEvents;
  UINT32                           AlgoIndex;
  TPMU_HA                          HashDigest;

  Print (L"EventLogFormat: (0x%x)\n", EventLogFormat);

  if (!CalculateExpected) {
    Print (L"Tcg2Event:\n");
    switch (EventLogFormat) {
    case EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2:
      EventHdr = (TCG_PCR_EVENT_HDR *)(UINTN)EventLogLocation;
      while ((UINTN)EventHdr <= EventLogLastEntry) {
        if ((PcrIndex == PCR_INDEX_ALL) || (PcrIndex == EventHdr->PCRIndex)) {
          DumpEvent (EventHdr);
        }
        EventHdr = (TCG_PCR_EVENT_HDR *)((UINTN)EventHdr + sizeof(TCG_PCR_EVENT_HDR) + EventHdr->EventSize);
      }

      if (FinalEventsTable == NULL) {
        Print (L"FinalEventsTable: NOT FOUND\n");
      } else {
        Print (L"FinalEventsTable:    (0x%x)\n", FinalEventsTable);
        Print (L"  Version:           (0x%x)\n", FinalEventsTable->Version);
        Print (L"  NumberOfEvents:    (0x%x)\n", FinalEventsTable->NumberOfEvents);

        EventHdr = (TCG_PCR_EVENT_HDR *)(UINTN)(FinalEventsTable + 1);
        for (NumberOfEvents = 0; NumberOfEvents < FinalEventsTable->NumberOfEvents; NumberOfEvents++) {
          if ((PcrIndex == PCR_INDEX_ALL) || (PcrIndex == EventHdr->PCRIndex)) {
            DumpEvent (EventHdr);
          }
          EventHdr = (TCG_PCR_EVENT_HDR *)((UINTN)EventHdr + sizeof(TCG_PCR_EVENT_HDR) + EventHdr->EventSize);
        }
      }
      break;

    case EFI_TCG2_EVENT_LOG_FORMAT_TCG_2:
      EventHdr = (TCG_PCR_EVENT_HDR *)(UINTN)EventLogLocation;
      DumpEvent (EventHdr);
      TcgEfiSpecIdEventStruct = (TCG_EfiSpecIDEventStruct *)(EventHdr + 1);
      DumpTcgEfiSpecIdEventStruct (TcgEfiSpecIdEventStruct);

      TcgPcrEvent2 = (TCG_PCR_EVENT2 *)((UINTN)TcgEfiSpecIdEventStruct + GetTcgEfiSpecIdEventStructSize (TcgEfiSpecIdEventStruct));
      while ((UINTN)TcgPcrEvent2 <= EventLogLastEntry) {
        if ((PcrIndex == PCR_INDEX_ALL) || (PcrIndex == TcgPcrEvent2->PCRIndex)) {
          DumpEvent2 (TcgPcrEvent2);
        }
        TcgPcrEvent2 = (TCG_PCR_EVENT2 *)((UINTN)TcgPcrEvent2 + GetPcrEvent2Size (TcgPcrEvent2));
      }

      if (FinalEventsTable == NULL) {
        Print (L"FinalEventsTable: NOT FOUND\n");
      } else {
        Print (L"FinalEventsTable:    (0x%x)\n", FinalEventsTable);
        Print (L"  Version:           (0x%x)\n", FinalEventsTable->Version);
        Print (L"  NumberOfEvents:    (0x%x)\n", FinalEventsTable->NumberOfEvents);

        TcgPcrEvent2 = (TCG_PCR_EVENT2 *)(UINTN)(FinalEventsTable + 1);

        for (NumberOfEvents = 0; NumberOfEvents < FinalEventsTable->NumberOfEvents; NumberOfEvents++) {
          if ((PcrIndex == PCR_INDEX_ALL) || (PcrIndex == TcgPcrEvent2->PCRIndex)) {
            DumpEvent2 (TcgPcrEvent2);
          }
          TcgPcrEvent2 = (TCG_PCR_EVENT2 *)((UINTN)TcgPcrEvent2 + GetPcrEvent2Size (TcgPcrEvent2));
        }
      }

      break;
    }
    Print (L"Tcg2Event end\n");
  } else {
    switch (EventLogFormat) {
    case EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2:
      ZeroMem (&TcgDigest, sizeof(TcgDigest));
      EventHdr = (TCG_PCR_EVENT_HDR *)(UINTN)EventLogLocation;
      while ((UINTN)EventHdr <= EventLogLastEntry) {
        if ((PcrIndex == EventHdr->PCRIndex) && (EventHdr->EventType != EV_NO_ACTION)) {
          ExtendEvent (TPM_ALG_SHA1, &TcgDigest, &EventHdr->Digest);
        }
        EventHdr = (TCG_PCR_EVENT_HDR *)((UINTN)EventHdr + sizeof(TCG_PCR_EVENT_HDR) + EventHdr->EventSize);
      }

      if (FinalEventsTable != NULL) {
        EventHdr = (TCG_PCR_EVENT_HDR *)(UINTN)(FinalEventsTable + 1);
        for (NumberOfEvents = 0; NumberOfEvents < FinalEventsTable->NumberOfEvents; NumberOfEvents++) {
          if ((PcrIndex == EventHdr->PCRIndex) && (EventHdr->EventType != EV_NO_ACTION)) {
            ExtendEvent (TPM_ALG_SHA1, &TcgDigest, &EventHdr->Digest);
          }
          EventHdr = (TCG_PCR_EVENT_HDR *)((UINTN)EventHdr + sizeof(TCG_PCR_EVENT_HDR) + EventHdr->EventSize);
        }
      }

      Print (L"Tcg2Event Calculated:\n");
      Print (L"    PCRIndex  - %d\n", PcrIndex);
      Print (L"    Digest    - ");
      for (Index = 0; Index < sizeof(TCG_DIGEST); Index++) {
        Print (L"%02x", TcgDigest.digest[Index]);
      }
      Print (L"\n");
      DumpPcr (PcrIndex, TPM_ALG_SHA1);
      break;

    case EFI_TCG2_EVENT_LOG_FORMAT_TCG_2:
      EventHdr = (TCG_PCR_EVENT_HDR *)(UINTN)EventLogLocation;
      TcgEfiSpecIdEventStruct = (TCG_EfiSpecIDEventStruct *)(EventHdr + 1);

      numberOfAlgorithms = GetTcgSpecIdNumberOfAlgorithms (TcgEfiSpecIdEventStruct);
      digestSize = GetTcgSpecIdDigestSize (TcgEfiSpecIdEventStruct);
      for (AlgoIndex = 0; AlgoIndex < numberOfAlgorithms; AlgoIndex++) {
        HashAlg = digestSize[AlgoIndex].algorithmId;
        ZeroMem (&HashDigest, sizeof(HashDigest));

        TcgPcrEvent2 = (TCG_PCR_EVENT2 *)((UINTN)TcgEfiSpecIdEventStruct + GetTcgEfiSpecIdEventStructSize (TcgEfiSpecIdEventStruct));
        while ((UINTN)TcgPcrEvent2 <= EventLogLastEntry) {
          if ((PcrIndex == TcgPcrEvent2->PCRIndex) && (TcgPcrEvent2->EventType != EV_NO_ACTION)) {
            DigestBuffer = GetDigestFromPcrEvent2 (TcgPcrEvent2, HashAlg);
            if (DigestBuffer != NULL) {
              ExtendEvent (HashAlg, HashDigest.sha1, DigestBuffer);
            }
          }
          TcgPcrEvent2 = (TCG_PCR_EVENT2 *)((UINTN)TcgPcrEvent2 + GetPcrEvent2Size (TcgPcrEvent2));
        }

        if (FinalEventsTable != NULL) {
          TcgPcrEvent2 = (TCG_PCR_EVENT2 *)(UINTN)(FinalEventsTable + 1);
          for (NumberOfEvents = 0; NumberOfEvents < FinalEventsTable->NumberOfEvents; NumberOfEvents++) {
            if ((PcrIndex == TcgPcrEvent2->PCRIndex) && (TcgPcrEvent2->EventType != EV_NO_ACTION)) {
              DigestBuffer = GetDigestFromPcrEvent2 (TcgPcrEvent2, HashAlg);
              if (DigestBuffer != NULL) {
                ExtendEvent (HashAlg, HashDigest.sha1, DigestBuffer);
              }
            }
            TcgPcrEvent2 = (TCG_PCR_EVENT2 *)((UINTN)TcgPcrEvent2 + GetPcrEvent2Size (TcgPcrEvent2));
          }
        }

        Print (L"Tcg2Event Calculated:\n");
        Print (L"    PCRIndex  - %d\n", PcrIndex);
        Print (L"    Digest    - ");
        for (Index = 0; Index < digestSize[AlgoIndex].digestSize; Index++) {
          Print (L"%02x", HashDigest.sha1[Index]);
        }
        Print (L"\n");
        DumpPcr (PcrIndex, HashAlg);
      }
      break;
    }
  }
}

VOID
DumpTcg2Capability (
  IN EFI_TCG2_BOOT_SERVICE_CAPABILITY     *ProtocolCapability
  )
{
  Print (L"Tcg2 Capability:\n");
  Print (L"  Size                - 0x%02x\n", ProtocolCapability->Size);
  Print (L"  StructureVersion    - %02x.%02x\n", ProtocolCapability->StructureVersion.Major, ProtocolCapability->StructureVersion.Minor);
  Print (L"  ProtocolVersion     - %02x.%02x\n", ProtocolCapability->StructureVersion.Major, ProtocolCapability->StructureVersion.Minor);
  Print (L"  HashAlgorithmBitmap - 0x%08x\n", ProtocolCapability->HashAlgorithmBitmap);
  Print (L"  SupportedEventLogs  - 0x%08x\n", ProtocolCapability->SupportedEventLogs);
  Print (L"  TPMPresentFlag      - 0x%02x\n", ProtocolCapability->TPMPresentFlag);
  Print (L"  MaxCommandSize      - 0x%04x\n", ProtocolCapability->MaxCommandSize);
  Print (L"  MaxResponseSize     - 0x%04x\n", ProtocolCapability->MaxResponseSize);
  Print (L"  ManufacturerID      - 0x%08x\n", ProtocolCapability->ManufacturerID);
  if ((ProtocolCapability->ProtocolVersion.Major > 0x01) || 
      ((ProtocolCapability->ProtocolVersion.Major == 0x01) && ((ProtocolCapability->ProtocolVersion.Minor > 0x00)))) {
    Print (L"  NumberOfPCRBanks    - 0x%08x\n", ProtocolCapability->NumberOfPCRBanks);
    Print (L"  ActivePcrBanks      - 0x%08x\n", ProtocolCapability->ActivePcrBanks);
  }
  return ;
}

/**
  This function print usage.
**/
VOID
PrintUsage (
  VOID
  )
{
  Print (
    L"Tcg2DumpLog Version 0.1\n"
    L"Copyright (C) Intel Corp 2015. All rights reserved.\n"
    L"\n"
    );
  Print (
    L"Tcg2DumpLog in EFI Shell Environment.\n"
    L"\n"
    L"usage: Tcg2DumpLog [-I <PcrIndex>] [-L <LogFormat>] [-E]\n"
    L"usage: Tcg2DumpLog [-C]\n"
    );
  Print (
    L"  -I   - PcrIndex, the valid value is 0-23|ALL (case sensitive)\n"
    L"  -L   - LogFormat, the bitmask of EventLogFormat (Hex based)\n"
    L"  -E   - Print expected PCR value\n"
    L"  -C   - Dump Tcg2 Capability\n"
    );
  return;
}

/**
  The driver's entry point.

  @param[in] ImageHandle  The firmware allocated handle for the EFI image.  
  @param[in] SystemTable  A pointer to the EFI System Table.
  
  @retval EFI_SUCCESS     The entry point is executed successfully.
  @retval other           Some error occurs when executing this entry point.
**/
EFI_STATUS
EFIAPI
UefiMain (
  IN    EFI_HANDLE                  ImageHandle,
  IN    EFI_SYSTEM_TABLE            *SystemTable
  )
{
  EFI_STATUS                       Status;
  LIST_ENTRY                       *ParamPackage;
  CHAR16                           *PcrIndexName;
  UINT32                           PcrIndex;
  BOOLEAN                          CalculateExpected;
  EFI_TCG2_PROTOCOL                *Tcg2Protocol;
  EFI_PHYSICAL_ADDRESS             EventLogLocation;
  EFI_PHYSICAL_ADDRESS             EventLogLastEntry;
  BOOLEAN                          EventLogTruncated;
  UINTN                            Index;
  UINT32                           LogFormat;
  CHAR16                           *LogFormatName;
  EFI_TCG2_BOOT_SERVICE_CAPABILITY ProtocolCapability;
  EFI_TCG2_FINAL_EVENTS_TABLE      *FinalEventsTable;

  Status = ShellCommandLineParse (mParamList, &ParamPackage, NULL, TRUE);
  if (EFI_ERROR(Status)) {
    Print(L"ERROR: Incorrect command line.\n");
    return Status;
  }

  if (ParamPackage == NULL ||
     ShellCommandLineGetFlag(ParamPackage, L"-?") ||
     ShellCommandLineGetFlag(ParamPackage, L"-h")) {
    PrintUsage ();
    return EFI_SUCCESS;
  }

  //
  // Get PcrIndex
  //
  PcrIndexName = (CHAR16 *)ShellCommandLineGetValue(ParamPackage, L"-I");
  if (PcrIndexName == NULL) {
    PcrIndex = PCR_INDEX_ALL;
  } else {
    if (StrCmp (PcrIndexName, L"ALL") == 0) {
      PcrIndex = PCR_INDEX_ALL;
    } else {
      PcrIndex = (UINT32)StrDecimalToUintn (PcrIndexName);
      if (PcrIndex > MAX_PCR_INDEX) {
        Print (L"ERROR: PcrIndex too large (%d)!\n", PcrIndex);
        return EFI_NOT_FOUND;
      }
    }
  }
  
  //
  // Get LogFormat
  //
  LogFormatName = (CHAR16 *)ShellCommandLineGetValue(ParamPackage, L"-L");
  if (LogFormatName == NULL) {
    LogFormat = 0xFFFFFFFF;
  } else {
    LogFormat = (UINT32)StrHexToUintn (LogFormatName);
  }

  //
  // If we need calculate expected value
  //
  CalculateExpected = ShellCommandLineGetFlag(ParamPackage, L"-E");

  //
  // Get Tcg2
  //
  Status = gBS->LocateProtocol (&gEfiTcg2ProtocolGuid, NULL, (VOID **) &Tcg2Protocol);
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: Locate Tcg2Protocol - %r\n", Status);
    return Status;
  }

  ZeroMem (&ProtocolCapability, sizeof(ProtocolCapability));
  ProtocolCapability.Size = sizeof(ProtocolCapability);
  Status = Tcg2Protocol->GetCapability (
                           Tcg2Protocol,
                           &ProtocolCapability
                           );
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: Tcg2Protocol->GetCapability - %r\n", Status);
    return Status;
  }

  //
  // Dump capability
  //
  if (ShellCommandLineGetFlag(ParamPackage, L"-C")) {
    DumpTcg2Capability (&ProtocolCapability);
    if ((ProtocolCapability.ProtocolVersion.Major < 0x01) || 
        ((ProtocolCapability.ProtocolVersion.Major == 0x01) && ((ProtocolCapability.ProtocolVersion.Minor == 0x00)))) {
    } else {
      UINT32           PCRBanks;
      Status = Tcg2Protocol->GetActivePcrBanks (
                               Tcg2Protocol,
                               &PCRBanks
                               );
      if (!EFI_ERROR (Status)) {
        Print (L"CurrentActivePCRBanks - 0x%08x\n", PCRBanks);
      }
    }

    return EFI_SUCCESS;
  }

  for (Index = 0; Index < sizeof(mTcg2EventInfo)/sizeof(mTcg2EventInfo[0]); Index++) {
    if ((mTcg2EventInfo[Index].LogFormat & LogFormat) != 0) {
      Status = Tcg2Protocol->GetEventLog (
                               Tcg2Protocol,
                               mTcg2EventInfo[Index].LogFormat,
                               &EventLogLocation,
                               &EventLogLastEntry,
                               &EventLogTruncated
                               );
      if (EFI_ERROR (Status)) {
        Print (L"ERROR: Tcg2Protocol->GetEventLog(0x%x) - %r\n", mTcg2EventInfo[Index].LogFormat, Status);
        continue;
      }
      if (EventLogTruncated) {
        Print (L"WARNING: EventLogTruncated\n");
      }

      FinalEventsTable = NULL;
      if (mTcg2EventInfo[Index].LogFormat == EFI_TCG2_EVENT_LOG_FORMAT_TCG_2) {
        EfiGetSystemConfigurationTable (&gEfiTcg2FinalEventsTableGuid, (VOID **)&FinalEventsTable);
      }

      //
      // DumpLog
      //
      if (CalculateExpected && (PcrIndex == PCR_INDEX_ALL)) {
        for (PcrIndex = 0; PcrIndex <= MAX_PCR_INDEX; PcrIndex++) {
          DumpEventLog (mTcg2EventInfo[Index].LogFormat, EventLogLocation, EventLogLastEntry, FinalEventsTable, PcrIndex, CalculateExpected);
        }
      } else {
        DumpEventLog (mTcg2EventInfo[Index].LogFormat, EventLogLocation, EventLogLastEntry, FinalEventsTable, PcrIndex, CalculateExpected);
      }
    }
  }
  return EFI_SUCCESS;
}
