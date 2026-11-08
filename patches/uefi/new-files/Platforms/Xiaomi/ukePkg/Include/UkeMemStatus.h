/** @file
  Shared header for UkeBootMemoryDxe status table.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef UKE_MEM_STATUS_H_
#define UKE_MEM_STATUS_H_

#define UKE_MEM_STATUS_MAGIC    0x554B454DU  /* "UKEM" */
#define UKE_MEM_STATUS_VERSION  1

#define UKE_MEM_DTB_HOB         1

#pragma pack(1)
typedef struct {
  UINT32  Magic;
  UINT32  Version;
  UINT32  DtbSource;
  UINT32  DtbSize;
  UINT64  Dtb;
  UINT64  InitrdStart;
  UINT64  InitrdEnd;
  UINT32  InitrdPublished;
} UKE_MEM_STATUS;
#pragma pack()

#endif
