/*
 * HLLib
 * Copyright (C) 2006-2010 Ryan Gregg

 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later
 * version.
 */

#include "HLLib.h"
#include "ZIPFile.h"
#include "Streams.h"
#include "Checksum.h"

using namespace HLLib;

#define HL_ZIP_LOCAL_FILE_HEADER_SIGNATURE					0x04034b50
#define HL_ZIP_FILE_HEADER_SIGNATURE						0x02014b50
#define HL_ZIP_END_OF_CENTRAL_DIRECTORY_RECORD_SIGNATURE	0x06054b50

#define HL_ZIP_CHECKSUM_LENGTH								0x00008000

const char *CZIPFile::lpAttributeNames[] = { "Disk", "Comment" };
const char *CZIPFile::lpItemAttributeNames[] = { "Create Version", "Extract Version", "Flags", "Compression Method", "CRC", "Disk", "Comment" };

CZIPFile::CZIPFile() : CPackage(), pFileHeaderView(0), pEndOfCentralDirectoryRecordView(0), pEndOfCentralDirectoryRecord(0)
{

}

CZIPFile::~CZIPFile()
{
	this->Close();
}

HLPackageType CZIPFile::GetType() const
{
	return HL_PACKAGE_ZIP;
}

const hlChar *CZIPFile::GetExtension() const
{
	return "zip";
}

const hlChar *CZIPFile::GetDescription() const
{
	return "Zip File";
}

hlBool CZIPFile::MapDataStructures()
{
	if(sizeof(ZIPEndOfCentralDirectoryRecord) > this->pMapping->GetMappingSize())
	{
		LastError.SetErrorMessage("Invalid file: the file map is too small for it's header.");
		return hlFalse;
	}

	hlUInt uiTest;
	hlULongLong uiOffset = 0, uiLength = this->pMapping->GetMappingSize();
	while(uiOffset < uiLength - sizeof(uiTest))
	{
		Mapping::CView *pTestView = 0;

		if(!this->pMapping->Map(pTestView, uiOffset, sizeof(hlUInt)))
		{
			return hlFalse;
		}

		uiTest = *(hlUInt *)pTestView->GetView();

		this->pMapping->Unmap(pTestView);

		switch(uiTest)
		{
			case HL_ZIP_END_OF_CENTRAL_DIRECTORY_RECORD_SIGNATURE:
			{
				if(!this->pMapping->Map(pTestView, uiOffset, sizeof(ZIPEndOfCentralDirectoryRecord)))
				{
					return hlFalse;
				}

				const ZIPEndOfCentralDirectoryRecord EndOfCentralDirRecord = *static_cast<const ZIPEndOfCentralDirectoryRecord *>(pTestView->GetView());

				this->pMapping->Unmap(pTestView);

				if(!this->pMapping->Map(this->pEndOfCentralDirectoryRecordView, uiOffset, sizeof(ZIPEndOfCentralDirectoryRecord) + EndOfCentralDirRecord.uiCommentLength))
				{
					return hlFalse;
				}

				this->pEndOfCentralDirectoryRecord = static_cast<const ZIPEndOfCentralDirectoryRecord *>(this->pEndOfCentralDirectoryRecordView->GetView());

				if(!this->pMapping->Map(this->pFileHeaderView, this->pEndOfCentralDirectoryRecord->uiStartOfCentralDirOffset, this->pEndOfCentralDirectoryRecord->uiCentralDirectorySize))
				{
					return hlFalse;
				}

				return hlTrue;
			}
			case HL_ZIP_FILE_HEADER_SIGNATURE:
			{
				if(!this->pMapping->Map(pTestView, uiOffset, sizeof(ZIPFileHeader)))
				{
					return hlFalse;
				}

				const ZIPFileHeader FileHeader = *static_cast<const ZIPFileHeader *>(pTestView->GetView());

				this->pMapping->Unmap(pTestView);

				uiOffset += static_cast<hlULongLong>(sizeof(ZIPFileHeader) + FileHeader.uiFileNameLength + FileHeader.uiExtraFieldLength + FileHeader.uiFileCommentLength);
				break;
			}
			case HL_ZIP_LOCAL_FILE_HEADER_SIGNATURE:
			{
				if(!this->pMapping->Map(pTestView, uiOffset, sizeof(ZIPLocalFileHeader)))
				{
					return hlFalse;
				}

				const ZIPLocalFileHeader LocalFileHeader = *static_cast<const ZIPLocalFileHeader *>(pTestView->GetView());

				this->pMapping->Unmap(pTestView);

				uiOffset += static_cast<hlULongLong>(sizeof(ZIPLocalFileHeader) + LocalFileHeader.uiFileNameLength + LocalFileHeader.uiExtraFieldLength + LocalFileHeader.uiCompressedSize);
				break;
			}
			default:
			{
				LastError.SetErrorMessageFormated("Invalid file: unknown section signature %#.8x.", uiTest);
				return hlFalse;
			}
		}
	}

	LastError.SetErrorMessage("Invalid file: unexpected end of file while scanning for end of central directory record.");
	return hlFalse;
}

hlVoid CZIPFile::UnmapDataStructures()
{
	this->pMapping->Unmap(this->pFileHeaderView);

	this->pEndOfCentralDirectoryRecord = 0;
	this->pMapping->Unmap(this->pEndOfCentralDirectoryRecordView);
}

CDirectoryFolder *CZIPFile::CreateRoot()
{
	CDirectoryFolder *pRoot = new CDirectoryFolder(this);

	hlUInt uiTest, uiOffset = 0;
	while(uiOffset < this->pEndOfCentralDirectoryRecord->uiCentralDirectorySize - sizeof(uiTest))
	{
		uiTest = *(hlUInt *)((hlByte *)this->pFileHeaderView->GetView() + uiOffset);

		switch(uiTest)
		{
			case HL_ZIP_FILE_HEADER_SIGNATURE:
			{
				ZIPFileHeader *pFileHeader = static_cast<ZIPFileHeader *>((hlVoid *)((hlByte *)this->pFileHeaderView->GetView() + uiOffset));

				hlChar *lpFileName = new hlChar[pFileHeader->uiFileNameLength + 1];
				memcpy(lpFileName, (hlByte *)pFileHeader + sizeof(ZIPFileHeader), pFileHeader->uiFileNameLength);
				lpFileName[pFileHeader->uiFileNameLength] = '\0';

				// Check if we have just a file, or if the file has directories we need to create.
				if(strchr(lpFileName, '/') == 0 && strchr(lpFileName, '\\') == 0)
				{
					pRoot->AddFile(lpFileName, HL_ID_INVALID, pFileHeader);
				}
				else
				{
					// Tokenize the file path and create the directories.
					CDirectoryFolder *pInsertFolder = pRoot;

					hlChar lpTemp[256] = "";
					hlChar *lpToken = strtok(lpFileName, "/\\");
					while(lpToken != 0)
					{
						strcpy(lpTemp, lpToken);

						lpToken = strtok(0, "/\\");

						if(lpToken != 0)
						{
							// Check if the directory exists.
							CDirectoryItem *pItem = pInsertFolder->GetItem(lpTemp);
							if(pItem == 0 || pItem->GetType() == HL_ITEM_FILE)
							{
								// It doesn't, create it.
								pInsertFolder = pInsertFolder->AddFolder(lpTemp);
							}
							else
							{
								// It does, use it.
								pInsertFolder = static_cast<CDirectoryFolder *>(pItem);
							}
						}
					}

					// The file name is the last token, add it.
					pInsertFolder->AddFile(lpTemp, HL_ID_INVALID, pFileHeader);
				}

				delete []lpFileName;

				uiOffset += sizeof(ZIPFileHeader) + pFileHeader->uiFileNameLength + pFileHeader->uiExtraFieldLength + pFileHeader->uiFileCommentLength;
				break;
			}
			default:
			{
				uiOffset = this->pEndOfCentralDirectoryRecord->uiCentralDirectorySize;
				break;
			}
		}
	}

	return pRoot;
}

hlUInt CZIPFile::GetAttributeCountInternal() const
{
	return HL_ZIP_PACKAGE_COUNT;
}

const hlChar *CZIPFile::GetAttributeNameInternal(HLPackageAttribute eAttribute) const
{
	if(eAttribute < HL_ZIP_PACKAGE_COUNT)
	{
		return this->lpAttributeNames[eAttribute];
	}

	return 0;
}

hlBool CZIPFile::GetAttributeInternal(HLPackageAttribute eAttribute, HLAttribute &Attribute) const
{
	switch(eAttribute)
	{
		case HL_ZIP_PACKAGE_DISK:
		{
			hlAttributeSetUnsignedInteger(&Attribute, this->lpAttributeNames[eAttribute], this->pEndOfCentralDirectoryRecord->uiNumberOfThisDisk, hlFalse);
			return hlTrue;
		}
		case HL_ZIP_PACKAGE_COMMENT:
		{
			hlChar *lpComment = new hlChar[this->pEndOfCentralDirectoryRecord->uiCommentLength + 1];
			memcpy(lpComment, (hlByte *)this->pEndOfCentralDirectoryRecord + sizeof(ZIPEndOfCentralDirectoryRecord), this->pEndOfCentralDirectoryRecord->uiCommentLength);
			lpComment[this->pEndOfCentralDirectoryRecord->uiCommentLength] = '\0';

			hlAttributeSetString(&Attribute, this->lpAttributeNames[eAttribute], lpComment);

			delete []lpComment;
			return hlTrue;
		}
		default:
		{
			return hlFalse;
		}
	}
}

hlUInt CZIPFile::GetItemAttributeCountInternal() const
{
	return HL_ZIP_ITEM_COUNT;
}

const hlChar *CZIPFile::GetItemAttributeNameInternal(HLPackageAttribute eAttribute) const
{
	if(eAttribute < HL_ZIP_ITEM_COUNT)
	{
		return this->lpItemAttributeNames[eAttribute];
	}

	return 0;
}

hlBool CZIPFile::GetItemAttributeInternal(const CDirectoryItem *pItem, HLPackageAttribute eAttribute, HLAttribute &Attribute) const
{
	switch(pItem->GetType())
	{
		case HL_ITEM_FILE:
		{
			const CDirectoryFile *pFile = static_cast<const CDirectoryFile *>(pItem);
			const ZIPFileHeader *pDirectoryItem = static_cast<const ZIPFileHeader *>(pFile->GetData());
			switch(eAttribute)
			{
				case HL_ZIP_ITEM_CREATE_VERSION:
				{
					hlAttributeSetUnsignedInteger(&Attribute, this->lpItemAttributeNames[eAttribute], pDirectoryItem->uiVersionMadeBy, hlFalse);
					return hlTrue;
				}
				case HL_ZIP_ITEM_EXTRACT_VERSION:
				{
					hlAttributeSetUnsignedInteger(&Attribute, this->lpItemAttributeNames[eAttribute], pDirectoryItem->uiVersionNeededToExtract, hlFalse);
					return hlTrue;
				}
				case HL_ZIP_ITEM_FLAGS:
				{
					hlAttributeSetUnsignedInteger(&Attribute, this->lpItemAttributeNames[eAttribute], pDirectoryItem->uiFlags, hlTrue);
					return hlTrue;
				}
				case HL_ZIP_ITEM_COMPRESSION_METHOD:
				{
					hlAttributeSetUnsignedInteger(&Attribute, this->lpItemAttributeNames[eAttribute], pDirectoryItem->uiCompressionMethod, hlTrue);
					return hlTrue;
				}
				case HL_ZIP_ITEM_CRC:
				{
					hlAttributeSetUnsignedInteger(&Attribute, this->lpItemAttributeNames[eAttribute], pDirectoryItem->uiCRC32, hlTrue);
					return hlTrue;
				}
				case HL_ZIP_ITEM_DISK:
				{
					hlAttributeSetUnsignedInteger(&Attribute, this->lpItemAttributeNames[eAttribute], pDirectoryItem->uiDiskNumberStart, hlFalse);
					return hlTrue;
				}
				case HL_ZIP_ITEM_COMMENT:
				{
					hlChar *lpComment = new hlChar[pDirectoryItem->uiFileCommentLength + 1];
					memcpy(lpComment, (hlByte *)pDirectoryItem + sizeof(ZIPFileHeader) + pDirectoryItem->uiFileNameLength + pDirectoryItem->uiExtraFieldLength, pDirectoryItem->uiFileCommentLength);
					lpComment[pDirectoryItem->uiFileCommentLength] = '\0';

					hlAttributeSetString(&Attribute, this->lpItemAttributeNames[eAttribute], lpComment);

					delete []lpComment;
					return hlTrue;
				}
			}
			break;
		}
	}

	return hlFalse;
}

hlBool CZIPFile::GetFileExtractableInternal(const CDirectoryFile *pFile, hlBool &bExtractable) const
{
	const ZIPFileHeader *pDirectoryItem = static_cast<const ZIPFileHeader *>(pFile->GetData());

	bExtractable = pDirectoryItem->uiCompressionMethod == 0 && pDirectoryItem->uiDiskNumberStart == this->pEndOfCentralDirectoryRecord->uiNumberOfThisDisk;

	return hlTrue;
}

hlBool CZIPFile::GetFileValidationInternal(const CDirectoryFile *pFile, HLValidation &eValidation) const
{
	const ZIPFileHeader *pDirectoryItem = static_cast<const ZIPFileHeader *>(pFile->GetData());

	if(pDirectoryItem->uiCompressionMethod != 0 || pDirectoryItem->uiDiskNumberStart != this->pEndOfCentralDirectoryRecord->uiNumberOfThisDisk)
	{
		eValidation = HL_VALIDATES_ASSUMED_OK;
		return hlTrue;
	}

	hlULong uiChecksum = 0;
	Streams::IStream *pStream = 0;
	if(const_cast<CZIPFile *>(this)->CreateStreamInternal(pFile, pStream))
	{
		if(pStream->Open(HL_MODE_READ))
		{
			hlULongLong uiTotalBytes = 0, uiFileBytes = pStream->GetStreamSize();
			hlUInt uiBufferSize;
			hlByte lpBuffer[HL_ZIP_CHECKSUM_LENGTH];

			hlBool bCancel = hlFalse;
			hlValidateFileProgress(const_cast<CDirectoryFile *>(pFile), uiTotalBytes, uiFileBytes, &bCancel);

			while((uiBufferSize = pStream->Read(lpBuffer, sizeof(lpBuffer))) != 0)
			{
				if(bCancel)
				{
					eValidation = HL_VALIDATES_CANCELED;
					break;
				}

				uiChecksum = CRC32(lpBuffer, uiBufferSize, uiChecksum);

				uiTotalBytes += static_cast<hlULongLong>(uiBufferSize);

				hlValidateFileProgress(const_cast<CDirectoryFile *>(pFile), uiTotalBytes, uiFileBytes, &bCancel);
			}

			pStream->Close();
		}

		const_cast<CZIPFile *>(this)->ReleaseStreamInternal(*pStream);
		delete pStream;
	}

	eValidation = (hlULong)pDirectoryItem->uiCRC32 == uiChecksum ? HL_VALIDATES_OK : HL_VALIDATES_CORRUPT;

	return hlTrue;
}

hlBool CZIPFile::GetFileSizeInternal(const CDirectoryFile *pFile, hlUInt &uiSize) const
{
	const ZIPFileHeader *pDirectoryItem = static_cast<const ZIPFileHeader *>(pFile->GetData());

	uiSize = pDirectoryItem->uiUncompressedSize;

	return hlTrue;
}

hlBool CZIPFile::GetFileSizeOnDiskInternal(const CDirectoryFile *pFile, hlUInt &uiSize) const
{
	const ZIPFileHeader *pDirectoryItem = static_cast<const ZIPFileHeader *>(pFile->GetData());

	uiSize = pDirectoryItem->uiCompressedSize;

	return hlTrue;
}

hlBool CZIPFile::CreateStreamInternal(const CDirectoryFile *pFile, Streams::IStream *&pStream) const
{
	const ZIPFileHeader *pDirectoryItem = static_cast<const ZIPFileHeader *>(pFile->GetData());

	if(pDirectoryItem->uiCompressionMethod != 0)
	{
		LastError.SetErrorMessageFormated("Compression format %#.2x not supported.", pDirectoryItem->uiCompressionMethod);
		return hlFalse;
	}

	if(pDirectoryItem->uiDiskNumberStart != this->pEndOfCentralDirectoryRecord->uiNumberOfThisDisk)
	{
		LastError.SetErrorMessageFormated("File resides on disk %u.", pDirectoryItem->uiDiskNumberStart);
		return hlFalse;
	}

	Mapping::CView *pDirectoryEnrtyView = 0;

	if(!this->pMapping->Map(pDirectoryEnrtyView, pDirectoryItem->uiRelativeOffsetOfLocalHeader, sizeof(ZIPLocalFileHeader)))
	{
		return hlFalse;
	}

	const ZIPLocalFileHeader DirectoryEntry = *static_cast<const ZIPLocalFileHeader *>(pDirectoryEnrtyView->GetView());

	this->pMapping->Unmap(pDirectoryEnrtyView);

	if(DirectoryEntry.uiSignature != HL_ZIP_LOCAL_FILE_HEADER_SIGNATURE)
	{
		LastError.SetErrorMessageFormated("Invalid file data offset.", pDirectoryItem->uiDiskNumberStart);
		return hlFalse;
	}

	pStream = new Streams::CMappingStream(*this->pMapping, pDirectoryItem->uiRelativeOffsetOfLocalHeader + sizeof(ZIPLocalFileHeader) + DirectoryEntry.uiFileNameLength + DirectoryEntry.uiExtraFieldLength, DirectoryEntry.uiUncompressedSize);

	return hlTrue;
}
