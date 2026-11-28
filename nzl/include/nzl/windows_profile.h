#pragma once

#ifndef _WIN32
#error "nzl/windows_profile.h is Windows-only"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "nzl/game_profile.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nzl::windows_profile {

enum class FileCheckStatus {
  Match,
  InvalidProfile,
  ProcessOpenFailed,
  ProcessPathUnavailable,
  GameAssemblyNotFound,
  FileOpenFailed,
  FileSizeMismatch,
  HashFailed,
  HashMismatch,
};

struct FileCheckResult {
  FileCheckStatus status = FileCheckStatus::InvalidProfile;
  std::filesystem::path path{};
  std::uint64_t actual_size = 0;
  std::string actual_sha256{};
  DWORD win32_error = ERROR_SUCCESS;
  LONG bcrypt_status = 0;

  bool matched() const { return status == FileCheckStatus::Match; }
};

inline const char* status_name(FileCheckStatus status) {
  switch (status) {
    case FileCheckStatus::Match: return "MATCH";
    case FileCheckStatus::InvalidProfile: return "INVALID_PROFILE";
    case FileCheckStatus::ProcessOpenFailed: return "PROCESS_OPEN_FAILED";
    case FileCheckStatus::ProcessPathUnavailable: return "PROCESS_PATH_UNAVAILABLE";
    case FileCheckStatus::GameAssemblyNotFound: return "GAMEASSEMBLY_FILE_NOT_FOUND";
    case FileCheckStatus::FileOpenFailed: return "GAMEASSEMBLY_FILE_OPEN_FAILED";
    case FileCheckStatus::FileSizeMismatch: return "FILE_SIZE_MISMATCH";
    case FileCheckStatus::HashFailed: return "SHA256_FAILED";
    case FileCheckStatus::HashMismatch: return "SHA256_MISMATCH";
  }
  return "UNKNOWN";
}

struct AlgorithmGuard {
  BCRYPT_ALG_HANDLE handle = nullptr;
  ~AlgorithmGuard() {
    if (handle) BCryptCloseAlgorithmProvider(handle, 0);
  }
};

struct HashGuard {
  BCRYPT_HASH_HANDLE handle = nullptr;
  ~HashGuard() {
    if (handle) BCryptDestroyHash(handle);
  }
};

struct FileGuard {
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~FileGuard() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};

inline bool process_image_path(DWORD pid, std::filesystem::path& out,
                               DWORD& error) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) {
    error = GetLastError();
    return false;
  }

  std::vector<wchar_t> buffer(32768, L'\0');
  DWORD chars = static_cast<DWORD>(buffer.size());
  const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &chars);
  error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(process);
  if (!ok || chars == 0) return false;
  out = std::filesystem::path(std::wstring(buffer.data(), chars));
  return true;
}

inline std::filesystem::path locate_gameassembly(DWORD pid, DWORD& error) {
  std::filesystem::path executable;
  if (!process_image_path(pid, executable, error)) return {};

  const std::filesystem::path directory = executable.parent_path();
  const std::wstring data_directory = executable.stem().wstring() + L"_Data";
  const std::array<std::filesystem::path, 3> candidates = {
      directory / L"GameAssembly.dll",
      directory / data_directory / L"Plugins" / L"x86_64" / L"GameAssembly.dll",
      directory / data_directory / L"Plugins" / L"GameAssembly.dll"};
  for (const auto& candidate : candidates) {
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      error = ERROR_SUCCESS;
      return candidate;
    }
  }

  error = ERROR_FILE_NOT_FOUND;
  return candidates.front();
}

inline bool sha256_file(const std::filesystem::path& path, std::string& hex,
                        DWORD& win32_error, LONG& bcrypt_status) {
  FileGuard file;
  file.handle = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file.handle == INVALID_HANDLE_VALUE) {
    win32_error = GetLastError();
    return false;
  }

  AlgorithmGuard algorithm;
  bcrypt_status = static_cast<LONG>(BCryptOpenAlgorithmProvider(
      &algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
  if (bcrypt_status < 0) return false;

  ULONG object_length = 0;
  ULONG property_bytes = 0;
  bcrypt_status = static_cast<LONG>(BCryptGetProperty(
      algorithm.handle, BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
      &property_bytes, 0));
  if (bcrypt_status < 0 || object_length == 0) return false;

  std::vector<UCHAR> object(object_length);
  HashGuard hash;
  bcrypt_status = static_cast<LONG>(BCryptCreateHash(
      algorithm.handle, &hash.handle, object.data(), object_length, nullptr, 0, 0));
  if (bcrypt_status < 0) return false;

  std::array<UCHAR, 1u << 20> buffer{};
  for (;;) {
    DWORD bytes_read = 0;
    if (!ReadFile(file.handle, buffer.data(),
                  static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
      win32_error = GetLastError();
      return false;
    }
    if (bytes_read == 0) break;
    bcrypt_status = static_cast<LONG>(BCryptHashData(
        hash.handle, buffer.data(), bytes_read, 0));
    if (bcrypt_status < 0) return false;
  }

  std::array<UCHAR, 32> digest{};
  bcrypt_status = static_cast<LONG>(BCryptFinishHash(
      hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0));
  if (bcrypt_status < 0) return false;

  static constexpr char kHex[] = "0123456789abcdef";
  hex.resize(digest.size() * 2);
  for (std::size_t i = 0; i < digest.size(); ++i) {
    hex[i * 2] = kHex[digest[i] >> 4];
    hex[i * 2 + 1] = kHex[digest[i] & 0x0f];
  }
  win32_error = ERROR_SUCCESS;
  return true;
}

inline bool equal_ascii_case_insensitive(std::string_view left,
                                         std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    char a = left[i];
    char b = right[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

inline FileCheckResult verify_gameassembly_file(
    DWORD pid, const game_profile::GameProfile& profile) {
  FileCheckResult result;
  if (!game_profile::fingerprint_configured(profile)) {
    result.status = FileCheckStatus::InvalidProfile;
    return result;
  }

  DWORD error = ERROR_SUCCESS;
  result.path = locate_gameassembly(pid, error);
  if (error != ERROR_SUCCESS) {
    result.status = (error == ERROR_FILE_NOT_FOUND)
                        ? FileCheckStatus::GameAssemblyNotFound
                        : FileCheckStatus::ProcessPathUnavailable;
    result.win32_error = error;
    return result;
  }

  FileGuard file;
  file.handle = CreateFileW(result.path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file.handle == INVALID_HANDLE_VALUE) {
    result.status = FileCheckStatus::FileOpenFailed;
    result.win32_error = GetLastError();
    return result;
  }

  LARGE_INTEGER file_size{};
  if (!GetFileSizeEx(file.handle, &file_size) || file_size.QuadPart < 0) {
    result.status = FileCheckStatus::FileOpenFailed;
    result.win32_error = GetLastError();
    return result;
  }
  result.actual_size = static_cast<std::uint64_t>(file_size.QuadPart);
  if (result.actual_size != profile.fingerprint.gameassembly_file_size) {
    result.status = FileCheckStatus::FileSizeMismatch;
    return result;
  }

  // Close the size-check handle before the streaming hash opens the same file.
  CloseHandle(file.handle);
  file.handle = INVALID_HANDLE_VALUE;
  if (!sha256_file(result.path, result.actual_sha256,
                   result.win32_error, result.bcrypt_status)) {
    result.status = FileCheckStatus::HashFailed;
    return result;
  }
  if (!equal_ascii_case_insensitive(result.actual_sha256,
                                    profile.fingerprint.gameassembly_sha256)) {
    result.status = FileCheckStatus::HashMismatch;
    return result;
  }

  result.status = FileCheckStatus::Match;
  return result;
}

template <typename ReadFn>
inline bool read_loaded_pe_size_of_image(std::uint64_t base, ReadFn&& read,
                                         std::uint32_t& size_of_image) {
  auto read_at = [&](std::uint64_t offset, void* output, std::size_t size) {
    if (base > UINT64_MAX - offset) return false;
    return read(base + offset, output, size);
  };

  std::uint16_t mz = 0;
  std::uint32_t pe_offset = 0;
  if (!read_at(0, &mz, sizeof(mz)) || mz != 0x5A4D ||
      !read_at(0x3C, &pe_offset, sizeof(pe_offset)) ||
      pe_offset < 0x40 || pe_offset >= 0x1000) return false;

  const std::uint64_t pe = pe_offset;
  std::uint32_t signature = 0;
  std::uint16_t optional_header_size = 0;
  std::uint16_t magic = 0;
  if (!read_at(pe, &signature, sizeof(signature)) || signature != 0x00004550 ||
      !read_at(pe + 20, &optional_header_size, sizeof(optional_header_size)) ||
      optional_header_size < 60 ||
      !read_at(pe + 24, &magic, sizeof(magic)) || magic != 0x020B) return false;

  std::uint32_t image_size = 0;
  if (!read_at(pe + 24 + 56, &image_size, sizeof(image_size)) ||
      image_size < sizeof(std::uint64_t)) return false;
  size_of_image = image_size;
  return true;
}

}  // namespace nzl::windows_profile
