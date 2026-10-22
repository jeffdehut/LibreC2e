#pragma once

#include "common/io/FileWriter.h"
#include <filesystem>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

// Commit a native companion file only after all bytes have been written and closed.
// Keep the previous version on failure, including disk-full and permission errors.
inline void write_persistent_file(const std::filesystem::path& path, const uint8_t* data, size_t size) {
	auto temporary = path;
	temporary += ".tmp";
	try {
		FileWriter writer(temporary);
		writer.write(data, size);
		writer.close();
#ifdef _WIN32
		if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			throw std::filesystem::filesystem_error("Replacing companion file", temporary, path,
				std::error_code(GetLastError(), std::system_category()));
#else
		std::filesystem::rename(temporary, path);
#endif
	} catch (...) {
		std::error_code ignored;
		std::filesystem::remove(temporary, ignored);
		throw;
	}
}

inline void write_persistent_file(const std::filesystem::path& path, const std::string& text) {
	write_persistent_file(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}
