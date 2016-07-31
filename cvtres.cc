/*
clang++ -std=c++11 -o cvtres cvtres.cc

A reimplemenation of cvtres.exe
*/
#include <experimental/string_view>
#include <map>
#include <vector>

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void fatal(const char* msg, ...) {
  va_list args;
  va_start(args, msg);
  vfprintf(stderr, msg, args);
  va_end(args);
  exit(1);
}

static uint32_t read_little_long(unsigned char** d) {
  uint32_t r = ((*d)[3] << 24) | ((*d)[2] << 16) | ((*d)[1] << 8) | (*d)[0];
  *d += sizeof(uint32_t);
  return r;
}

static uint16_t read_little_short(unsigned char** d) {
  uint16_t r = ((*d)[1] << 8) | (*d)[0];
  *d += sizeof(uint16_t);
  return r;
}

struct ResEntry {
  uint32_t data_size;
  uint32_t header_size;  // Always 0x20 plus storage for type_str and name_str
                         // if type or name aren't numeric.

  bool type_is_id;  // determines which of the following two is valid.
  uint16_t type_id;
  std::vector<char16_t> type_str;

  bool name_is_id;  // determines which of the following two is valid.
  uint16_t name_id;
  std::vector<char16_t> name_str;

  uint32_t data_version;
  uint16_t memory_flags;
  uint16_t language_id;
  uint32_t version;
  uint32_t characteristics;

  uint8_t* data;  // weak
  // XXX make this owned (?)
  //std::unique_ptr<uint8_t[]> data;
};

struct ResEntries {
  std::vector<ResEntry> entries;
};

static ResEntry load_resource_entry(uint8_t* data, uint32_t* n_read) {
  ResEntry entry;
  entry.data_size = read_little_long(&data);
  entry.header_size = read_little_long(&data);

  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms648027(v=vs.85).aspx

  // if type, name start with 0xffff then they're numeric IDs. Else they're
  // inline zero-terminated utf-16le strings. After name, there might be one
  // word of padding to align data_version.
  uint8_t* string_start = data;
  uint16_t type = read_little_short(&data);
  entry.type_is_id = type == 0xffff;
  if (entry.type_is_id) {
    entry.type_id = read_little_short(&data);
  } else {
    while (type != 0) {
      entry.type_str.push_back(type);
      type = read_little_short(&data);
    }
  }
  uint16_t name = read_little_short(&data);
  entry.name_is_id = name == 0xffff;
  if (entry.name_is_id) {
    entry.name_id = read_little_short(&data);
  } else {
    while (name != 0) {
      entry.name_str.push_back(name);
      name = read_little_short(&data);
    }
  }
  // Pad to dword boundary:
  if ((data - string_start) & 2)
    data += 2;
  // Check that bigger headers are explained by string types and names.
  if (entry.header_size != 0x20 + (data - string_start - 8))
    fatal("unexpected header size\n");  // XXX error code

  entry.data_version = read_little_long(&data);
  entry.memory_flags = read_little_short(&data);
  entry.language_id = read_little_short(&data);
  entry.version = read_little_long(&data);
  entry.characteristics = read_little_long(&data);

  entry.data = data;

  uint32_t total_size = entry.data_size + entry.header_size;
  *n_read = total_size + ((4 - (total_size & 3)) & 3);  // DWORD-align.
  return entry;
}

typedef struct {
  uint16_t Machine;
  uint16_t NumberOfSections;
  uint32_t TimeDateStamp;
  uint32_t PointerToSymbolTable;
  uint32_t NumberOfSymbols;
  uint16_t SizeOfOptionalHeader;
  uint16_t Characteristics;
} FileHeader;

#pragma pack(push, 1)
typedef struct {
  char Name[8];
  uint32_t Value;
  int16_t SectionNumber;  // 1-based index, or a special value (0, -1, -2)
  uint16_t Type;
  uint8_t StorageClass;
  uint8_t NumberOfAuxSymbols;
} StandardSymbolRecord;
_Static_assert(sizeof(StandardSymbolRecord) == 18, "");
#pragma pack(pop)

typedef struct {
  char Name[8];
  uint32_t VirtualSize;
  uint32_t VirtualAddress;
  uint32_t SizeOfRawData;
  uint32_t PointerToRawData;
  uint32_t PointerToRelocations;
  uint32_t PointerToLineNumbers;
  uint16_t NumberOfRelocations;
  uint16_t NumberOfLinenumbers;
  uint32_t Characteristics;
} SectionHeader;

#pragma pack(push, 1)
typedef struct {
  uint32_t VirtualAddress;
  uint32_t SymbolTableInd;  // zero-based
  uint16_t Type;
} Relocation;
_Static_assert(sizeof(Relocation) == 10, "");
#pragma pack(pop)

typedef struct {
  uint32_t Characteristics;
  uint32_t TimeDateStamp;
  uint16_t MajorVersion;
  uint16_t MinorVersion;
  uint16_t NumberOfNameEntries;
  uint16_t NumberOfIdEntries;
} ResourceDirectoryHeader;  // 16 bytes

typedef struct {
  uint32_t TypeNameLang;  // Either string address or id.
  // High bit 0: Address of a Resource Data Entry (a leaf).
  // High bit 1: Address of a Resource Directory Table.
  uint32_t DataRVA;
} ResourceDirectoryEntry;  // 8 bytes

typedef struct {
  uint32_t DataRVA;
  uint32_t Size;
  uint32_t Codepage;
  uint32_t Reserved;
} ResourceDataEntry;

struct NodeKey {
  bool is_id;  // determines which of the following two is valid.
  uint16_t id;
  const std::vector<char16_t>* str;

  bool operator<(const NodeKey& rhs) const {
    if (is_id != rhs.is_id)
      return is_id < rhs.is_id;  // Names come before ids.
    // If we come here, *this and rhs are either both numeric or both strings.
    if (is_id)
      return id < rhs.id;
    return *str < *rhs.str;
  }
};

static void write_rsrc_obj(const char* out_name, const ResEntries& entries) {
  // Want:
  // symbol table with relocations and section names
  // .rsrc$01 section with tree metadata (type->name->lang)
  //   - relocations for all ResourceDataEntries (ResourceDirectoryEntry leaves)
  // .rsrc$02 section with actual resource data

  FileHeader coff_header = {};
  coff_header.Machine = 0x8664;  // FIXME: /arch: flag for picking 32 or 64 bit
  coff_header.NumberOfSections = 2;  // .rsrc$01, .rsrc$02
  coff_header.TimeDateStamp = 0;  // FIXME: need flag, for inc linking with link
  // Symbols for section names have 1 aux entry each: (XXX)
  coff_header.NumberOfSymbols = 0; // 2*2 + entries.entries.size();
  //   - XXX is @comp.id and @feat.00 needed?
  coff_header.SizeOfOptionalHeader = 0;
  coff_header.Characteristics = 0x100;  // XXX

  // XXX write symbol table, followed by string table size ("4" means none,
  // because string table size includes size of the size field itself)

  // Write .rsrc$01 section.

  // The COFF spec says that the layout is:
  // - ResourceDirectoryHeaders each followed by its ResourceDirectoryEntries
  // - Strings. Each string is (len, chars).
  // - ResourceDataEntries (aligned)
  // - Actual resource data.
  //
  // cvtres.exe however writes data in this order:
  // - ResourceDirectoryHeaders each followed by its ResourceDirectoryEntries
  // - ResourceDataEntries (aligned)
  // - Strings. Each string is (len, chars).
  // - Relocations.
  // - Actual resource data.
  //
  // Match cvtres.exe's order.
  // For the tables, cvtres.exe writes all type headers, then all name headers,
  // then all lang headers (instead of depth-first).

  // Header.
 
  // Relocations.

  // Build type->name->lang resource tree.
  std::vector<uint16_t> string_table;
  std::map<std::experimental::u16string_view, uint32_t> strings;
  //std::map<std::pair<const ResEntry*, bool>, size_t> string_offset;

  std::map<NodeKey, std::map<NodeKey, std::map<uint16_t, const ResEntry*>>>
      directory;
  for (const auto& entry : entries.entries) {
    NodeKey type_key{entry.type_is_id, entry.type_id, &entry.type_str};
    NodeKey name_key{entry.name_is_id, entry.name_id, &entry.name_str};

    auto& lang_map = directory[type_key][name_key];
    auto lang_it = lang_map.insert(std::make_pair(entry.language_id, &entry));
    if (!lang_it.second)
      fatal("duplicate element");

    // Also write type and name to the string table.
    if (!entry.type_is_id) {
      std::experimental::u16string_view s(entry.type_str.data(),
                                          entry.type_str.size());
      auto it = strings.insert(std::make_pair(s, 0));
      if (it.second) {
fprintf(stderr, "adding type str\n");
        // String wasn't in |strings| yet.  Add it to the string table, and
        // update |it| to contain the right value.
        off_t index = string_table.size() * sizeof(uint16_t);
        string_table.push_back(entry.type_str.size());
        string_table.insert(string_table.end(), entry.type_str.begin(),
                            entry.type_str.end());
        it.first->second = index;
      }
      //string_offset[std::make_pair(&entry, false)] = it.first->second;
    }
    if (!entry.name_is_id) {
      std::experimental::u16string_view s(entry.name_str.data(),
                                          entry.name_str.size());
      auto it = strings.insert(std::make_pair(s, 0));
      if (it.second) {
fprintf(stderr, "adding name str\n");
        // String wasn't in |strings| yet.  Add it to the string table, and
        // update |it| to contain the right value.
        off_t index = string_table.size() * sizeof(uint16_t);
        string_table.push_back(entry.name_str.size());
        string_table.insert(string_table.end(), entry.name_str.begin(),
                            entry.name_str.end());
        it.first->second = index;
      }
      //string_offset[std::make_pair(&entry, true)] = it.first->second;
    }
  }

  // Do tree layout pass.
  std::vector<uint32_t> offsets;
  uint32_t offset = sizeof(ResourceDirectoryHeader) +
                    directory.size() * sizeof(ResourceDirectoryEntry);
  for (auto& type : directory) {
    offsets.push_back(offset);
    offset += sizeof(ResourceDirectoryHeader) +
              type.second.size() * sizeof(ResourceDirectoryEntry);
  }
  for (auto& type : directory) {
    for (auto& name : type.second) {
      offsets.push_back(offset);
      offset += sizeof(ResourceDirectoryHeader) +
                name.second.size() * sizeof(ResourceDirectoryEntry);
    }
  }
  //for (auto& type : directory) {
  //  for (auto& name : type.second) {
  //    for (size_t i = 0; i < name.second.size(); ++i) {
  //      offsets.push_back(offset);
  //      offset +=
  //          sizeof(ResourceDirectoryHeader) + sizeof(ResourceDirectoryEntry);
  //    }
  //  }
  //}
  uint32_t string_table_start = offset + entries.entries.size() * sizeof(ResourceDataEntry);
fprintf(stderr, "string table at 0x%x, %zu bytes long\n", string_table_start, sizeof(uint16_t) * string_table.size());

  // Layout is:
  size_t num_types = directory.size();
  size_t num_named_types = 0;
  for (const auto& types : directory) {
    if (types.first.is_id)
      break;
    ++num_named_types;
  }

  ResourceDirectoryHeader type_dir = {};
  type_dir.NumberOfNameEntries = num_named_types;
  type_dir.NumberOfIdEntries= num_types - num_named_types;
  //write(type_dir);  // XXX
  unsigned next_offset_index = 0;
  for (auto& type : directory) {
    ResourceDirectoryEntry entry;
    entry.DataRVA = offsets[next_offset_index++] | 0x80000000;
    if (!type.first.is_id) {
      std::experimental::u16string_view s(type.first.str->data(),
                                          type.first.str->size());
      auto it = strings.find(s);
      if (it == strings.end())
        fatal("type str should have been inserted above!\n");
      entry.TypeNameLang = string_table_start + it->second;
      // XXX cvtres.exe sets high bit of TypeNameLang for strings; should we?
    } else {
      entry.TypeNameLang = (type.first.id << 16) | 0xffff;
    }
fprintf(stderr, "%x -> %x\n", entry.TypeNameLang, entry.DataRVA);
  }

  for (auto& type : directory) {
    size_t num_names = directory.size();
    size_t num_named_names = 0;
    for (const auto& names : type.second) {
      if (names.first.is_id)
        break;
      ++num_named_names;
    }

    ResourceDirectoryHeader name_dir = {};
    name_dir.NumberOfNameEntries = num_named_names;
    name_dir.NumberOfIdEntries = num_names - num_named_names;
    for (auto& name : type.second) {
      ResourceDirectoryEntry entry;
      entry.DataRVA = offsets[next_offset_index++] | 0x80000000;
      if (!name.first.is_id) {
        std::experimental::u16string_view s(name.first.str->data(),
                                            name.first.str->size());
        auto it = strings.find(s);
        if (it == strings.end())
          fatal("name str should have been inserted above!\n");
        entry.TypeNameLang = string_table_start + it->second;
      // XXX cvtres.exe sets high bit of TypeNameLang for strings; should we?
      } else {
        entry.TypeNameLang = (name.first.id << 16) | 0xffff;
      }
fprintf(stderr, "%x -> %x\n", entry.TypeNameLang, entry.DataRVA);
    }
  }

  unsigned int data_index = 0;
  for (auto& type : directory) {
    for (auto& name : type.second) {
      ResourceDirectoryHeader lang_dir = {};
      lang_dir.NumberOfNameEntries = 0;
      lang_dir.NumberOfIdEntries = name.second.size();
      for (auto& lang : name.second) {
        ResourceDirectoryEntry entry;
        entry.DataRVA = offset + data_index++ * sizeof(ResourceDataEntry);
        entry.TypeNameLang = (lang.first << 16) | 0xffff;
fprintf(stderr, "%x -> %x\n", entry.TypeNameLang, entry.DataRVA);
      }
    }
  }

  // Write resource data entries (the COFF spec recommens to put these after
  // the string table, but cvtres.exe puts them before it).

  // Write string table after resource directory. (with padding)

  // Write .rsrc$02 section.

  // Header.

  // Actual resource data.
}

int main(int argc, char* argv[]) {
  if (argc != 2)
    fatal("Expected args == 2, got %d\n", argc);

  const char *in_name = argv[1];

  // Read input.
  int in_file = open(in_name, O_RDONLY);
  if (!in_file)
    fatal("Unable to read \'%s\'\n", in_name);

  struct stat in_stat;
  if (fstat(in_file, &in_stat))
    fatal("Failed to stat \'%s\'\n", in_name);

  uint8_t* data = (uint8_t*)mmap(/*addr=*/0, in_stat.st_size, PROT_READ,
                                 MAP_SHARED, in_file,
                                 /*offset=*/0);
  if (data == MAP_FAILED)
    fatal("Failed to mmap: %d (%s)\n", errno, strerror(errno));

  uint8_t* end = data + in_stat.st_size;

  ResEntries entries;
  bool is_first = true;
  while (data < end) {
    uint32_t n_read;
    ResEntry entry = load_resource_entry(data, &n_read);
    if (is_first) {
      // Ignore not-16-bit marker.
      is_first = false;
      if (!entry.type_is_id || entry.type_id != 0 || !entry.name_is_id ||
          entry.name_id != 0)
        fatal("expected not-16-bit marker as first entry\n");
    } else {
      if (entry.type_is_id && entry.type_id == 0)
        fatal("0 type\n");
      if (entry.name_is_id && entry.name_id == 0)
        fatal("0 name\n");
      entries.entries.push_back(std::move(entry));
    }
    data += n_read;
  }

  write_rsrc_obj("rsrc.obj", entries);

  munmap(data, in_stat.st_size);
  close(in_file);
}
