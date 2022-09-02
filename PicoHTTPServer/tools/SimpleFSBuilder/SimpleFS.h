#pragma once

typedef struct
{
	uint32_t FileSize;
	uint32_t NameOffset;
	uint32_t ContentTypeOffset;
	uint32_t DataOffset;
} StoredFileEntry;

typedef struct
{
	uint32_t Magic;
	uint32_t EntryCount;
	uint32_t NameBlockSize;
	uint32_t DataBlockSize;
} GlobalFSHeader;

enum 
{
	kSimpleFSHeaderMagic = '1SFS',
};
