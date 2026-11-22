#include "elf_dependencies.h"

#include <elf.h>
#include <fcntl.h>
#include <glib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace {

class ElfReader {
 public:
  ElfReader(int file, uint64_t size) : file_(file), size_(size) {}

  bool read(uint64_t offset, void* buffer, size_t length) const {
    if (offset > size_ || length > size_ - offset) return false;
    auto* out = static_cast<char*>(buffer);
    while (length > 0) {
      const ssize_t count = pread(file_, out, length, offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      offset += count;
      out += count;
      length -= count;
    }
    return true;
  }

 private:
  int file_;
  uint64_t size_;
};

struct ByteOrder {
  bool little;

  uint16_t operator()(uint16_t word) const {
    return little ? GUINT16_FROM_LE(word) : GUINT16_FROM_BE(word);
  }
  uint32_t operator()(uint32_t word) const {
    return little ? GUINT32_FROM_LE(word) : GUINT32_FROM_BE(word);
  }
  uint64_t operator()(uint64_t word) const {
    return little ? GUINT64_FROM_LE(word) : GUINT64_FROM_BE(word);
  }
};

template <typename Header, typename Segment, typename Dynamic>
std::vector<std::string> read_dependencies(const ElfReader& reader,
                                           ByteOrder word) {
  Header header{};
  if (!reader.read(0, &header, sizeof(header)) ||
      (word(header.e_type) != ET_EXEC && word(header.e_type) != ET_DYN) ||
      word(header.e_phentsize) != sizeof(Segment)) {
    return {};
  }
  const size_t segment_count = word(header.e_phnum);
  if (segment_count == 0 || segment_count > 4096) return {};
  std::vector<Segment> segments(segment_count);
  if (!reader.read(word(header.e_phoff), segments.data(),
                   segment_count * sizeof(Segment))) {
    return {};
  }
  const auto dynamic = std::find_if(segments.begin(), segments.end(),
      [&](const Segment& segment) { return word(segment.p_type) == PT_DYNAMIC; });
  if (dynamic == segments.end()) return {};
  const uint64_t dynamic_size = word(dynamic->p_filesz);
  if (dynamic_size == 0 || dynamic_size > 64 * 1024 ||
      dynamic_size % sizeof(Dynamic) != 0) {
    return {};
  }
  std::vector<Dynamic> entries(dynamic_size / sizeof(Dynamic));
  if (!reader.read(word(dynamic->p_offset), entries.data(), dynamic_size)) return {};

  uint64_t string_address = 0;
  uint64_t string_size = 0;
  bool terminated = false;
  std::vector<uint64_t> needed;
  for (const auto& entry : entries) {
    const auto tag = word(static_cast<decltype(entry.d_un.d_val)>(entry.d_tag));
    const auto value = word(entry.d_un.d_val);
    if (tag == DT_NULL) {
      terminated = true;
      break;
    }
    if (tag == DT_NEEDED) needed.push_back(value);
    if (tag == DT_STRTAB) string_address = value;
    if (tag == DT_STRSZ) string_size = value;
  }
  if (!terminated || needed.empty() || string_address == 0 || string_size == 0 ||
      string_size > 4 * 1024 * 1024) {
    return {};
  }

  // DT_STRTAB is a virtual address. Find its file offset in a load segment.
  for (const auto& segment : segments) {
    const uint64_t virtual_address = word(segment.p_vaddr);
    if (word(segment.p_type) != PT_LOAD || string_address < virtual_address) continue;
    const uint64_t delta = string_address - virtual_address;
    const uint64_t file_size = word(segment.p_filesz);
    const uint64_t offset = word(segment.p_offset);
    if (delta > file_size || string_size > file_size - delta ||
        delta > UINT64_MAX - offset) {
      continue;
    }
    std::vector<char> strings(string_size);
    if (!reader.read(offset + delta, strings.data(), strings.size())) return {};

    std::vector<std::string> result;
    for (const auto name_offset : needed) {
      if (name_offset >= strings.size()) continue;
      const char* name = strings.data() + name_offset;
      const auto length = std::min<uint64_t>(strings.size() - name_offset, 512);
      const auto* end = static_cast<const char*>(std::memchr(name, '\0', length));
      if (end != nullptr && end > name && g_utf8_validate(name, end - name, nullptr)) {
        result.emplace_back(name, end);
      }
    }
    return result;
  }
  return {};
}

std::vector<std::string> inspect(const ElfReader& reader) {
  unsigned char ident[EI_NIDENT];
  if (!reader.read(0, ident, sizeof(ident)) ||
      std::memcmp(ident, ELFMAG, SELFMAG) != 0 ||
      ident[EI_VERSION] != EV_CURRENT ||
      (ident[EI_DATA] != ELFDATA2LSB && ident[EI_DATA] != ELFDATA2MSB) ||
      (ident[8] == 'A' && ident[9] == 'I' && (ident[10] == 1 || ident[10] == 2))) {
    return {};
  }
  const ByteOrder word{ident[EI_DATA] == ELFDATA2LSB};
  if (ident[EI_CLASS] == ELFCLASS64) {
    return read_dependencies<Elf64_Ehdr, Elf64_Phdr, Elf64_Dyn>(reader, word);
  }
  if (ident[EI_CLASS] == ELFCLASS32) {
    return read_dependencies<Elf32_Ehdr, Elf32_Phdr, Elf32_Dyn>(reader, word);
  }
  return {};
}

}  // namespace

std::vector<std::string> read_elf_dependencies(const char* path) {
  const int file = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (file < 0) return {};
  struct stat info{};
  std::vector<std::string> dependencies;
  if (fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0) {
    dependencies = inspect(ElfReader(file, info.st_size));
  }
  close(file);
  return dependencies;
}
