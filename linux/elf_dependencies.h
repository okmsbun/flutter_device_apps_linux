#ifndef FLUTTER_DEVICE_APPS_LINUX_ELF_DEPENDENCIES_H_
#define FLUTTER_DEVICE_APPS_LINUX_ELF_DEPENDENCIES_H_

#include <string>
#include <vector>

std::vector<std::string> read_elf_dependencies(const char* path);

#endif
