/*
 *  ReactOS kernel
 *  Copyright (C) 2002,2003 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/attrib.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Valentin Verkhovsky
 *                   Hervé Poussineau (hpoussin@reactos.org)
 *                   Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

NTSTATUS
AddRun(PNTFS_ATTR_CONTEXT AttrContext,
       ULONGLONG NextAssignedCluster,
       ULONG RunLength)
{
    UNIMPLEMENTED;

    if (!AttrContext->Record.IsNonResident)
        return STATUS_INVALID_PARAMETER;

    return STATUS_NOT_IMPLEMENTED;
}

PUCHAR
DecodeRun(PUCHAR DataRun,
          LONGLONG *DataRunOffset,
          ULONGLONG *DataRunLength)
{
    UCHAR DataRunOffsetSize;
    UCHAR DataRunLengthSize;
    CHAR i;

    DataRunOffsetSize = (*DataRun >> 4) & 0xF;
    DataRunLengthSize = *DataRun & 0xF;
    *DataRunOffset = 0;
    *DataRunLength = 0;
    DataRun++;
    for (i = 0; i < DataRunLengthSize; i++)
    {
        *DataRunLength += ((ULONG64)*DataRun) << (i * 8);
        DataRun++;
    }

    /* NTFS 3+ sparse files */
    if (DataRunOffsetSize == 0)
    {
        *DataRunOffset = -1;
    }
    else
    {
        for (i = 0; i < DataRunOffsetSize - 1; i++)
        {
            *DataRunOffset += ((ULONG64)*DataRun) << (i * 8);
            DataRun++;
        }
        /* The last byte contains sign so we must process it different way. */
        *DataRunOffset = ((LONG64)(CHAR)(*(DataRun++)) << (i * 8)) + *DataRunOffset;
    }

    DPRINT("DataRunOffsetSize: %x\n", DataRunOffsetSize);
    DPRINT("DataRunLengthSize: %x\n", DataRunLengthSize);
    DPRINT("DataRunOffset: %x\n", *DataRunOffset);
    DPRINT("DataRunLength: %x\n", *DataRunLength);

    return DataRun;
}

BOOLEAN
FindRun(PNTFS_ATTR_RECORD NresAttr,
        ULONGLONG vcn,
        PULONGLONG lcn,
        PULONGLONG count)
{
    if (vcn < NresAttr->NonResident.LowestVCN || vcn > NresAttr->NonResident.HighestVCN)
        return FALSE;

    DecodeRun((PUCHAR)((ULONG_PTR)NresAttr + NresAttr->NonResident.MappingPairsOffset), (PLONGLONG)lcn, count);

    return TRUE;
}

static
NTSTATUS
InternalReadNonResidentAttributes(PFIND_ATTR_CONTXT Context)
{
    ULONGLONG ListSize;
    PNTFS_ATTR_RECORD Attribute;
    PNTFS_ATTR_CONTEXT ListContext;

    DPRINT("InternalReadNonResidentAttributes(%p)\n", Context);

    Attribute = Context->CurrAttr;
    ASSERT(Attribute->Type == AttributeAttributeList);

    if (Context->OnlyResident)
    {
        Context->NonResidentStart = NULL;
        Context->NonResidentEnd = NULL;
        return STATUS_SUCCESS;
    }

    if (Context->NonResidentStart != NULL)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    ListContext = PrepareAttributeContext(Attribute);
    ListSize = AttributeDataLength(&ListContext->Record);
    if (ListSize > 0xFFFFFFFF)
    {
        ReleaseAttributeContext(ListContext);
        return STATUS_BUFFER_OVERFLOW;
    }

    Context->NonResidentStart = ExAllocatePoolWithTag(NonPagedPool, (ULONG)ListSize, TAG_NTFS);
    if (Context->NonResidentStart == NULL)
    {
        ReleaseAttributeContext(ListContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ReadAttribute(Context->Vcb, ListContext, 0, (PCHAR)Context->NonResidentStart, (ULONG)ListSize) != ListSize)
    {
        ExFreePoolWithTag(Context->NonResidentStart, TAG_NTFS);
        Context->NonResidentStart = NULL;
        ReleaseAttributeContext(ListContext);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    ReleaseAttributeContext(ListContext);
    Context->NonResidentEnd = (PNTFS_ATTR_RECORD)((PCHAR)Context->NonResidentStart + ListSize);
    return STATUS_SUCCESS;
}

static
PNTFS_ATTR_RECORD
InternalGetNextAttribute(PFIND_ATTR_CONTXT Context)
{
    PNTFS_ATTR_RECORD NextAttribute;

    if (Context->CurrAttr == (PVOID)-1)
    {
        return NULL;
    }

    if (Context->CurrAttr >= Context->FirstAttr &&
        Context->CurrAttr < Context->LastAttr)
    {
        if (Context->CurrAttr->Length == 0)
        {
            DPRINT1("Broken length!\n");
            Context->CurrAttr = (PVOID)-1;
            return NULL;
        }

        NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Context->CurrAttr + Context->CurrAttr->Length);
        Context->Offset += ((ULONG_PTR)NextAttribute - (ULONG_PTR)Context->CurrAttr);
        Context->CurrAttr = NextAttribute;

        if (Context->CurrAttr < Context->LastAttr &&
            Context->CurrAttr->Type != AttributeEnd)
        {
            return Context->CurrAttr;
        }
    }

    if (Context->NonResidentStart == NULL)
    {
        Context->CurrAttr = (PVOID)-1;
        return NULL;
    }

    if (Context->CurrAttr < Context->NonResidentStart ||
        Context->CurrAttr >= Context->NonResidentEnd)
    {
        Context->CurrAttr = Context->NonResidentStart;
    }
    else if (Context->CurrAttr->Length != 0)
    {
        NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Context->CurrAttr + Context->CurrAttr->Length);
        Context->Offset += ((ULONG_PTR)NextAttribute - (ULONG_PTR)Context->CurrAttr);
        Context->CurrAttr = NextAttribute;
    }
    else
    {
        DPRINT1("Broken length!\n");
        Context->CurrAttr = (PVOID)-1;
        return NULL;
    }

    if (Context->CurrAttr < Context->NonResidentEnd &&
        Context->CurrAttr->Type != AttributeEnd)
    {
        return Context->CurrAttr;
    }

    Context->CurrAttr = (PVOID)-1;
    return NULL;
}

NTSTATUS
FindFirstAttribute(PFIND_ATTR_CONTXT Context,
                   PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   BOOLEAN OnlyResident,
                   PNTFS_ATTR_RECORD * Attribute)
{
    NTSTATUS Status;

    DPRINT("FindFistAttribute(%p, %p, %p, %p, %u, %p)\n", Context, Vcb, FileRecord, OnlyResident, Attribute);

    Context->Vcb = Vcb;
    Context->OnlyResident = OnlyResident;
    Context->FirstAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);
    Context->CurrAttr = Context->FirstAttr;
    Context->LastAttr = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->BytesInUse);
    Context->NonResidentStart = NULL;
    Context->NonResidentEnd = NULL;
    Context->Offset = FileRecord->AttributeOffset;

    if (Context->FirstAttr->Type == AttributeEnd)
    {
        Context->CurrAttr = (PVOID)-1;
        return STATUS_END_OF_FILE;
    }
    else if (Context->FirstAttr->Type == AttributeAttributeList)
    {
        Status = InternalReadNonResidentAttributes(Context);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        *Attribute = InternalGetNextAttribute(Context);
        if (*Attribute == NULL)
        {
            return STATUS_END_OF_FILE;
        }
    }
    else
    {
        *Attribute = Context->CurrAttr;
        Context->Offset = (UCHAR*)Context->CurrAttr - (UCHAR*)FileRecord;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
FindNextAttribute(PFIND_ATTR_CONTXT Context,
                  PNTFS_ATTR_RECORD * Attribute)
{
    NTSTATUS Status;

    DPRINT("FindNextAttribute(%p, %p)\n", Context, Attribute);

    *Attribute = InternalGetNextAttribute(Context);
    if (*Attribute == NULL)
    {
        return STATUS_END_OF_FILE;
    }

    if (Context->CurrAttr->Type != AttributeAttributeList)
    {
        return STATUS_SUCCESS;
    }

    Status = InternalReadNonResidentAttributes(Context);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    *Attribute = InternalGetNextAttribute(Context);
    if (*Attribute == NULL)
    {
        return STATUS_END_OF_FILE;
    }

    return STATUS_SUCCESS;
}

VOID
FindCloseAttribute(PFIND_ATTR_CONTXT Context)
{
    if (Context->NonResidentStart != NULL)
    {
        ExFreePoolWithTag(Context->NonResidentStart, TAG_NTFS);
        Context->NonResidentStart = NULL;
    }
}

static
VOID
NtfsDumpFileNameAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PFILENAME_ATTRIBUTE FileNameAttr;

    DbgPrint("  $FILE_NAME ");

//    DbgPrint(" Length %lu  Offset %hu ", Attribute->Resident.ValueLength, Attribute->Resident.ValueOffset);

    FileNameAttr = (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
    DbgPrint(" (%x) '%.*S' ", FileNameAttr->NameType, FileNameAttr->NameLength, FileNameAttr->Name);
    DbgPrint(" '%x' \n", FileNameAttr->FileAttributes);
    DbgPrint(" AllocatedSize: %I64u\nDataSize: %I64u\n", FileNameAttr->AllocatedSize, FileNameAttr->DataSize);
}


static
VOID
NtfsDumpStandardInformationAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PSTANDARD_INFORMATION StandardInfoAttr;

    DbgPrint("  $STANDARD_INFORMATION ");

//    DbgPrint(" Length %lu  Offset %hu ", Attribute->Resident.ValueLength, Attribute->Resident.ValueOffset);

    StandardInfoAttr = (PSTANDARD_INFORMATION)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
    DbgPrint(" '%x' ", StandardInfoAttr->FileAttribute);
}


static
VOID
NtfsDumpVolumeNameAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PWCHAR VolumeName;

    DbgPrint("  $VOLUME_NAME ");

//    DbgPrint(" Length %lu  Offset %hu ", Attribute->Resident.ValueLength, Attribute->Resident.ValueOffset);

    VolumeName = (PWCHAR)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
    DbgPrint(" '%.*S' ", Attribute->Resident.ValueLength / sizeof(WCHAR), VolumeName);
}


static
VOID
NtfsDumpVolumeInformationAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PVOLINFO_ATTRIBUTE VolInfoAttr;

    DbgPrint("  $VOLUME_INFORMATION ");

//    DbgPrint(" Length %lu  Offset %hu ", Attribute->Resident.ValueLength, Attribute->Resident.ValueOffset);

    VolInfoAttr = (PVOLINFO_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
    DbgPrint(" NTFS Version %u.%u  Flags 0x%04hx ",
             VolInfoAttr->MajorVersion,
             VolInfoAttr->MinorVersion,
             VolInfoAttr->Flags);
}


static
VOID
NtfsDumpIndexRootAttribute(PNTFS_ATTR_RECORD Attribute)
{
    PINDEX_ROOT_ATTRIBUTE IndexRootAttr;

    IndexRootAttr = (PINDEX_ROOT_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);

    if (IndexRootAttr->AttributeType == AttributeFileName)
        ASSERT(IndexRootAttr->CollationRule == COLLATION_FILE_NAME);

    DbgPrint("  $INDEX_ROOT (%uB, %u) ", IndexRootAttr->SizeOfEntry, IndexRootAttr->ClustersPerIndexRecord);

    if (IndexRootAttr->Header.Flags == INDEX_ROOT_SMALL)
    {
        DbgPrint(" (small) ");
    }
    else
    {
        ASSERT(IndexRootAttr->Header.Flags == INDEX_ROOT_LARGE);
        DbgPrint(" (large) ");
    }
}


static
VOID
NtfsDumpAttribute(PDEVICE_EXTENSION Vcb,
                  PNTFS_ATTR_RECORD Attribute)
{
    UNICODE_STRING Name;

    ULONGLONG lcn = 0;
    ULONGLONG runcount = 0;

    switch (Attribute->Type)
    {
        case AttributeFileName:
            NtfsDumpFileNameAttribute(Attribute);
            break;

        case AttributeStandardInformation:
            NtfsDumpStandardInformationAttribute(Attribute);
            break;

        case AttributeObjectId:
            DbgPrint("  $OBJECT_ID ");
            break;

        case AttributeSecurityDescriptor:
            DbgPrint("  $SECURITY_DESCRIPTOR ");
            break;

        case AttributeVolumeName:
            NtfsDumpVolumeNameAttribute(Attribute);
            break;

        case AttributeVolumeInformation:
            NtfsDumpVolumeInformationAttribute(Attribute);
            break;

        case AttributeData:
            DbgPrint("  $DATA ");
            //DataBuf = ExAllocatePool(NonPagedPool,AttributeLengthAllocated(Attribute));
            break;

        case AttributeIndexRoot:
            NtfsDumpIndexRootAttribute(Attribute);
            break;

        case AttributeIndexAllocation:
            DbgPrint("  $INDEX_ALLOCATION ");
            break;

        case AttributeBitmap:
            DbgPrint("  $BITMAP ");
            break;

        case AttributeReparsePoint:
            DbgPrint("  $REPARSE_POINT ");
            break;

        case AttributeEAInformation:
            DbgPrint("  $EA_INFORMATION ");
            break;

        case AttributeEA:
            DbgPrint("  $EA ");
            break;

        case AttributePropertySet:
            DbgPrint("  $PROPERTY_SET ");
            break;

        case AttributeLoggedUtilityStream:
            DbgPrint("  $LOGGED_UTILITY_STREAM ");
            break;

        default:
            DbgPrint("  Attribute %lx ",
                     Attribute->Type);
            break;
    }

    if (Attribute->Type != AttributeAttributeList)
    {
        if (Attribute->NameLength != 0)
        {
            Name.Length = Attribute->NameLength * sizeof(WCHAR);
            Name.MaximumLength = Name.Length;
            Name.Buffer = (PWCHAR)((ULONG_PTR)Attribute + Attribute->NameOffset);

            DbgPrint("'%wZ' ", &Name);
        }

        DbgPrint("(%s)\n",
                 Attribute->IsNonResident ? "non-resident" : "resident");

        if (Attribute->IsNonResident)
        {
            FindRun(Attribute,0,&lcn, &runcount);

            DbgPrint("  AllocatedSize %I64u  DataSize %I64u InitilizedSize %I64u\n",
                     Attribute->NonResident.AllocatedSize, Attribute->NonResident.DataSize, Attribute->NonResident.InitializedSize);
            DbgPrint("  logical clusters: %I64u - %I64u\n",
                     lcn, lcn + runcount - 1);
        }
    }
}


VOID
NtfsDumpFileAttributes(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER FileRecord)
{
    NTSTATUS Status;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;

    Status = FindFirstAttribute(&Context, Vcb, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        NtfsDumpAttribute(Vcb, Attribute);

        Status = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
}

PFILENAME_ATTRIBUTE
GetFileNameFromRecord(PDEVICE_EXTENSION Vcb,
                      PFILE_RECORD_HEADER FileRecord,
                      UCHAR NameType)
{
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    PFILENAME_ATTRIBUTE Name;
    NTSTATUS Status;

    Status = FindFirstAttribute(&Context, Vcb, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->Type == AttributeFileName)
        {
            Name = (PFILENAME_ATTRIBUTE)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
            if (Name->NameType == NameType ||
                (Name->NameType == NTFS_FILE_NAME_WIN32_AND_DOS && NameType == NTFS_FILE_NAME_WIN32) ||
                (Name->NameType == NTFS_FILE_NAME_WIN32_AND_DOS && NameType == NTFS_FILE_NAME_DOS))
            {
                FindCloseAttribute(&Context);
                return Name;
            }
        }

        Status = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
    return NULL;
}

NTSTATUS
GetLastClusterInDataRun(PDEVICE_EXTENSION Vcb, PNTFS_ATTR_RECORD Attribute, PULONGLONG LastCluster)
{
    LONGLONG DataRunOffset;
    ULONGLONG DataRunLength;
    LONGLONG DataRunStartLCN;

    ULONGLONG LastLCN = 0;
    PUCHAR DataRun = (PUCHAR)Attribute + Attribute->NonResident.MappingPairsOffset;

    if (!Attribute->IsNonResident)
        return STATUS_INVALID_PARAMETER;

    while (1)
    {
        DataRun = DecodeRun(DataRun, &DataRunOffset, &DataRunLength);
       
        if (DataRunOffset == -1)
        {
            // sparse run
            if (*DataRun == 0)
            {
                // if it's the last run, return the last cluster of the last run
                *LastCluster = LastLCN + DataRunLength - 1;
                break;
            }
        }
        else
        {
            // Normal data run.
            DataRunStartLCN = LastLCN + DataRunOffset;
            LastLCN = DataRunStartLCN;
        }             

        if (*DataRun == 0)
        {
            *LastCluster = LastLCN + DataRunLength - 1;
            break;
        }
    }

    return STATUS_SUCCESS;
}

PSTANDARD_INFORMATION
GetStandardInformationFromRecord(PDEVICE_EXTENSION Vcb,
                                 PFILE_RECORD_HEADER FileRecord)
{
    NTSTATUS Status;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    PSTANDARD_INFORMATION StdInfo;

    Status = FindFirstAttribute(&Context, Vcb, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(Status))
    {
        if (Attribute->Type == AttributeStandardInformation)
        {
            StdInfo = (PSTANDARD_INFORMATION)((ULONG_PTR)Attribute + Attribute->Resident.ValueOffset);
            FindCloseAttribute(&Context);
            return StdInfo;
        }

        Status = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
    return NULL;
}

PFILENAME_ATTRIBUTE
GetBestFileNameFromRecord(PDEVICE_EXTENSION Vcb,
                          PFILE_RECORD_HEADER FileRecord)
{
    PFILENAME_ATTRIBUTE FileName;

    FileName = GetFileNameFromRecord(Vcb, FileRecord, NTFS_FILE_NAME_POSIX);
    if (FileName == NULL)
    {
        FileName = GetFileNameFromRecord(Vcb, FileRecord, NTFS_FILE_NAME_WIN32);
        if (FileName == NULL)
        {
            FileName = GetFileNameFromRecord(Vcb, FileRecord, NTFS_FILE_NAME_DOS);
        }
    }

    return FileName;
}

/* EOF */