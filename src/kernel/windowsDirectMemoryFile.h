#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string>
#include <winioctl.h>

namespace Libs::LibKernel::Memory::Detail {

// Windows-only opt-in data-file owner; it never opens an existing backing file.
class WindowsDirectMemoryFile {
public:
	enum class Selection { PagefileDefault, FileReady, Rejected };
	WindowsDirectMemoryFile() = default;
	~WindowsDirectMemoryFile() { Close(); }
	WindowsDirectMemoryFile(const WindowsDirectMemoryFile&)            = delete;
	WindowsDirectMemoryFile& operator=(const WindowsDirectMemoryFile&) = delete;

	Selection OpenConfigured(uint64_t size) {
		std::array<wchar_t, 2048> directory {};
		SetLastError(ERROR_SUCCESS);
		const auto count = GetEnvironmentVariableW(L"KYTY_DIRECT_MEMORY_BACKING_DIR",
		                                           directory.data(), directory.size());
		if (count == 0 &&
		    (GetLastError() == ERROR_ENVVAR_NOT_FOUND || GetLastError() == ERROR_SUCCESS)) {
			return Selection::PagefileDefault;
		}
		m_requested = true;
		if (count == 0 || count >= directory.size()) {
			return Reject("environment-length", ERROR_INVALID_PARAMETER);
		}
		return Open(directory.data(), size);
	}

	// Explicit small-size seam for CPU-only ownership/mapping tests.
	Selection Open(const std::wstring& directory, uint64_t size) {
		m_requested = true;
		if (m_file != INVALID_HANDLE_VALUE || size == 0 || size > INT64_MAX ||
		    directory.size() < 3 || directory[1] != L':' || directory[2] != L'\\') {
			return Reject("directory-or-size", ERROR_INVALID_PARAMETER);
		}
		std::array<wchar_t, 2048> full {};
		const auto                full_count =
		    GetFullPathNameW(directory.c_str(), full.size(), full.data(), nullptr);
		if (full_count == 0 || full_count >= full.size()) {
			return Reject("directory-normalization",
			              full_count == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE);
		}
		std::wstring normalized(full.data());
		while (normalized.size() > 3 && normalized.back() == L'\\') {
			normalized.pop_back();
		}
		if (normalized.size() <= 3) {
			return Reject("private-subdirectory-required", ERROR_INVALID_PARAMETER);
		}
		std::array<wchar_t, 2048> volume {};
		std::array<wchar_t, 32>   filesystem {};
		DWORD                     volume_flags = 0;
		if (!GetVolumePathNameW(normalized.c_str(), volume.data(), volume.size())) {
			return Reject("volume-path", GetLastError());
		}
		if (GetDriveTypeW(volume.data()) != DRIVE_FIXED) {
			return Reject("local-fixed-volume-required", ERROR_NOT_SUPPORTED);
		}
		if (!GetVolumeInformationW(volume.data(), nullptr, 0, nullptr, nullptr, &volume_flags,
		                           filesystem.data(), filesystem.size())) {
			return Reject("volume-info", GetLastError());
		}
		if (_wcsicmp(filesystem.data(), L"NTFS") != 0 ||
		    (volume_flags & FILE_SUPPORTS_SPARSE_FILES) == 0) {
			return Reject("local-fixed-ntfs-required", ERROR_NOT_SUPPORTED);
		}
		// Check each ancestor before any file is created, rather than following a
		// junction and only discovering its destination after creation.
		for (size_t end = 3; end <= normalized.size(); ++end) {
			if (end != 3 && end != normalized.size() && normalized[end] != L'\\') {
				continue;
			}
			const auto prefix     = normalized.substr(0, end);
			const auto attributes = GetFileAttributesW(prefix.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES) {
				return Reject("directory-attributes", GetLastError());
			}
			if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
			    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
				return Reject("existing-non-reparse-directory-required", ERROR_PATH_NOT_FOUND);
			}
		}
		ULARGE_INTEGER available {};
		if (!GetDiskFreeSpaceExW(normalized.c_str(), &available, nullptr, nullptr)) {
			return Reject("disk-headroom-query", GetLastError());
		}
		if (available.QuadPart < size) {
			return Reject("disk-headroom", ERROR_DISK_FULL);
		}
		static std::atomic<uint64_t> sequence {0};
		for (uint32_t attempt = 0; attempt < 8; ++attempt) {
			m_path = normalized + L"\\kyty-direct-memory-" +
			         std::to_wstring(GetCurrentProcessId()) + L"-" +
			         std::to_wstring(GetTickCount64()) + L"-" +
			         std::to_wstring(sequence.fetch_add(1)) + L".tmp";
			m_file = CreateFileW(
			    m_path.c_str(), GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE, 0, nullptr,
			    CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
			if (m_file != INVALID_HANDLE_VALUE) {
				break;
			}
			const auto error = GetLastError();
			if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
				return Reject("create-private-file", error);
			}
		}
		if (m_file == INVALID_HANDLE_VALUE) {
			return Reject("unique-file-attempt-limit", ERROR_FILE_EXISTS);
		}
		// Reject parent junction/mount relocation after opening the fresh owned
		// file.
		std::array<wchar_t, 4096> final_path {};
		const auto                final_count = GetFinalPathNameByHandleW(
		    m_file, final_path.data(), final_path.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		const auto expected = L"\\\\?\\" + m_path;
		if (final_count == 0) {
			return Reject("final-owned-path-query", GetLastError());
		}
		if (final_count >= final_path.size() ||
		    _wcsicmp(final_path.data(), expected.c_str()) != 0) {
			return Reject("final-owned-path-mismatch", ERROR_ACCESS_DENIED);
		}
		DWORD returned = 0;
		if (!DeviceIoControl(m_file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
		                     nullptr)) {
			return Reject("set-sparse", GetLastError());
		}
		LARGE_INTEGER end {};
		end.QuadPart = static_cast<LONGLONG>(size);
		if (!SetFilePointerEx(m_file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(m_file)) {
			return Reject("set-size", GetLastError());
		}
		FILE_ZERO_DATA_INFORMATION zero {};
		zero.BeyondFinalZero.QuadPart = end.QuadPart;
		if (!DeviceIoControl(m_file, FSCTL_SET_ZERO_DATA, &zero, sizeof(zero), nullptr, 0,
		                     &returned, nullptr)) {
			return Reject("zero-entire-backing", GetLastError());
		}
		LARGE_INTEGER              actual {};
		BY_HANDLE_FILE_INFORMATION info {};
		if (!GetFileSizeEx(m_file, &actual) || !GetFileInformationByHandle(m_file, &info)) {
			return Reject("verify-file-query", GetLastError());
		}
		if (actual.QuadPart != end.QuadPart ||
		    (info.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0) {
			return Reject("verify-zero-file-metadata", ERROR_INVALID_DATA);
		}
		return Selection::FileReady;
	}

	[[nodiscard]] HANDLE              Handle() const { return m_file; }
	[[nodiscard]] bool                Requested() const { return m_requested; }
	[[nodiscard]] DWORD               Error() const { return m_error; }
	[[nodiscard]] const char*         Stage() const { return m_stage; }
	[[nodiscard]] const std::wstring& PathForTests() const { return m_path; }

private:
	void Close() {
		if (m_file != INVALID_HANDLE_VALUE) {
			CloseHandle(m_file);
			m_file = INVALID_HANDLE_VALUE;
		}
	}
	Selection Reject(const char* stage, DWORD error) {
		m_stage = stage;
		m_error = error;
		Close();
		return Selection::Rejected;
	}

	HANDLE       m_file      = INVALID_HANDLE_VALUE;
	bool         m_requested = false;
	DWORD        m_error     = ERROR_SUCCESS;
	const char*  m_stage     = "none";
	std::wstring m_path;
};

} // namespace Libs::LibKernel::Memory::Detail
