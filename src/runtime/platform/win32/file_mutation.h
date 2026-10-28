// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <string>
namespace venture::hle::libc::host::detail {
// Thin OS boundary permits deterministic unsupported-filesystem coverage.
using FileInformation = decltype(&SetFileInformationByHandle);
using FileAttributes = decltype(&GetFileAttributesW);
int unlink_posix(const std::wstring&, FileInformation = &SetFileInformationByHandle);
int rename_posix(const std::wstring&, const std::wstring&, FileInformation = &SetFileInformationByHandle,
                 FileAttributes = &GetFileAttributesW);
}
