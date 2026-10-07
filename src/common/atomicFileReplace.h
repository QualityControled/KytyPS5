#pragma once

#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Common {

// The caller must completely write, flush and close source first. Unlike File::RenameFile,
// this operation never deletes the existing destination before attempting replacement.
inline bool AtomicReplaceFile(const std::filesystem::path& source,
                              const std::filesystem::path& destination) {
#if defined(_WIN32)
	const auto source_wide = source.wstring();
	const auto destination_wide = destination.wstring();
	return MoveFileExW(source_wide.c_str(), destination_wide.c_str(),
	                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	std::error_code error;
	std::filesystem::rename(source, destination, error);
	return !error;
#endif
}

} // namespace Common
