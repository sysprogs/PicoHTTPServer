#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <list>
#include <exception>
#include <memory.h>
#include <string.h>
#include <map>
#include <vector>
#include "SimpleFS.h"

using namespace std;
using namespace std::filesystem;

struct TemporaryFileEntry
{
	string PathInArchive;
	string FullPath, Extension;
	uintmax_t Size;
	
	TemporaryFileEntry(const string &pathInArchive, const path &fullPath, uintmax_t size)
		: PathInArchive(pathInArchive),
		FullPath(fullPath.u8string()),
		Extension(fullPath.extension().u8string()),
		Size(size)
	{
		for (int i = 0; i < Extension.size(); i++)
			Extension[i] = tolower(Extension[i]);
	}
};

static std::string CombinePaths(std::string left, const path &right)
{
	if (!left.empty())
		left += "/";
	
	left += right.u8string();
	return left;
}

static void BuildFileListRecursively(path dir, std::list<TemporaryFileEntry> &entries, std::string pathBase)
{
	for (const auto &entry : directory_iterator(dir))
	{
		if (entry.is_directory())
			BuildFileListRecursively(entry.path(), entries, CombinePaths(pathBase, entry.path().filename()));
		else
		{
			path fn = entry.path().filename();
			if (!strcasecmp(fn.u8string().c_str(), "index.html"))
				fn = "";
			
			entries.emplace_back(CombinePaths(pathBase, fn), entry.path(), entry.file_size());
		}
	}

}
void WriteIfNotMatches(std::string fn, std::vector<char> &data)
{
	{
		std::ifstream fs(fn, ios::binary);
		std::vector<char> tmp(data.size());
		if (fs.readsome(tmp.data(), tmp.size()) == tmp.size())
		{
			if (tmp == data)
				return;
		}
	}
	
	std::ofstream fs(fn, ios::binary | ios::trunc);
	fs.write(data.data(), data.size());
}

struct ContentType
{
	std::string Value;
	int Offset;
	
	ContentType(const char *value)
		: Value(value)
	{
	}
};

int main(int argc, char *argv[])
{
	if (argc < 3)
	{
		cout << "Usage: SimpleFSBuilder <directory> <FS image>" << endl;
		return 1;
	}
	
	try
	{
		std::list<TemporaryFileEntry> entries;
		BuildFileListRecursively(argv[1], entries, "");
		GlobalFSHeader hdr = { kSimpleFSHeaderMagic, };
		
		map<string, ContentType> contentTypes = {
			{ ".txt", "text/plain" },
			{ ".htm", "text/html" },
			{ ".html", "text/html" },
			{ ".css", "text/css" },
			{ ".png", "image/png" },
			{ ".jpg", "image/jpeg" },
			{ ".svg", "image/svg+xml" },
		};

		for (const auto &entry : entries)
		{
			hdr.EntryCount++;
			hdr.NameBlockSize += entry.PathInArchive.size() + 1;
			hdr.DataBlockSize += entry.Size;
		}
		
		for (const auto &kv : contentTypes)
			hdr.NameBlockSize += kv.second.Value.size() + 1;
	
		std::vector<char> buffer(sizeof(GlobalFSHeader) + hdr.EntryCount * sizeof(StoredFileEntry) + hdr.NameBlockSize + hdr.DataBlockSize);
	
		*((GlobalFSHeader *)buffer.data()) = hdr;
		StoredFileEntry *storedEntries = (StoredFileEntry *)(buffer.data() + sizeof(GlobalFSHeader));
		char *names = (char *)(storedEntries + hdr.EntryCount);
		char *data = names + hdr.NameBlockSize;
	
		int i = 0, nameOff = 0, dataOff = 0;
		
		for (auto &kv : contentTypes)
		{
			kv.second.Offset = nameOff;
			memcpy(names + nameOff, kv.second.Value.c_str(), kv.second.Value.size() + 1);
			nameOff += kv.second.Value.size() + 1;
		}
	
		for (const auto &entry : entries)
		{
			storedEntries[i].FileSize = entry.Size;
			storedEntries[i].NameOffset = nameOff;
			storedEntries[i].DataOffset = dataOff;
			
			auto it = contentTypes.find(entry.Extension);
			if (it == contentTypes.end())
				it = contentTypes.find(".html");
			
			storedEntries[i].ContentTypeOffset = it->second.Offset;
			
			i++;
		
			ifstream ifs(entry.FullPath, ios::in | ios::binary);
			ifs.read(data + dataOff, entry.Size);
		
			memcpy(names + nameOff, entry.PathInArchive.c_str(), entry.PathInArchive.size() + 1);
		
			nameOff += entry.PathInArchive.size() + 1;
			dataOff += entry.Size;
		}
	
		if (nameOff != hdr.NameBlockSize)
			throw runtime_error("Unexpected name block size");
		if (dataOff != hdr.DataBlockSize)
			throw runtime_error("Unexpected data block size");
		if (i != hdr.EntryCount)
			throw runtime_error("Unexpected entry count");

		WriteIfNotMatches(argv[2], buffer);
		return 0;
	}
	catch (exception &ex)
	{
		cout << ex.what() << endl;
		return 1;
	}
}