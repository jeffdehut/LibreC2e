#include "FileWriter.h"

#include "common/io/IOException.h"

#include <string.h>
#include <exception>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static inline std::string errno_message() {
	return std::error_code(errno, std::generic_category()).message();
}

FileWriter::FileWriter(const fs::path& name) {
#ifdef _WIN32
	ptr_ = _wfopen(name.c_str(), L"wb");
#else
	ptr_ = fopen(name.c_str(), "wb");
#endif
	if (ptr_ == nullptr) {
		// TODO: better message
		// TODO: get error message on Windows
		throw IOException(fmt::format("failed to open file {:?}: {}",
			name.string(), errno_message()));
	}
	path_ = name;
}

FileWriter::FileWriter(const fs::path& name, FileWriter::option_append_t) {
#ifdef _WIN32
	ptr_ = _wfopen(name.c_str(), L"ab");
#else
	ptr_ = fopen(name.c_str(), "ab");
#endif
	if (ptr_ == nullptr) {
		// TODO: better message
		// TODO: get error message on Windows
		throw IOException(fmt::format("failed to open file {:?}: {}",
			name.string(), errno_message()));
	}
	path_ = name;
}

FileWriter::FileWriter(FileWriter&& other) {
	ptr_ = other.ptr_;
	path_ = other.path_;
	other.ptr_ = nullptr;
	other.path_ = fs::path{};
}

FileWriter::~FileWriter() {
	try {
		close();
	} catch (const std::exception& e) {
		// A destructor must not terminate the game during unwinding. Explicit close()
		// gives callers a recoverable error before removing an exported creature.
		fmt::print(stderr, "File close failed: {}\n", e.what());
	}
}

void FileWriter::close() {
	if (!ptr_) return;
	std::exception_ptr failure;
	try { flush(); } catch (...) { failure = std::current_exception(); }
	FILE* file = ptr_;
	ptr_ = nullptr;
	if (fclose(file) != 0 && !failure) {
		throw IOException(fmt::format("fclose() failed for file {:?}: {}", path_.string(), errno_message()));
	}
	if (failure) std::rethrow_exception(failure);
}

const fs::path& FileWriter::path() const {
	return path_;
}

void FileWriter::flush() {
	if (!ptr_) throw IOException("Cannot flush a closed file");
	if (fflush(ptr_) != 0) {
		throw IOException(fmt::format("fflush() failed for file {:?}: {}",
			path_.string(), errno_message()));
	}
#ifdef _WIN32
	if (_commit(_fileno(ptr_)) != 0) {
		throw IOException(fmt::format("_commit() failed for file {:?}: {}",
			path_.string(), errno_message()));
	}
#else
	if (fsync(fileno(ptr_)) != 0) {
		throw IOException(fmt::format("fsync() failed for file {:?}: {}",
			path_.string(), errno_message()));
	}
#endif
}


void FileWriter::do_write(const uint8_t* buf, size_t n) {
	if (!ptr_) throw IOException("Cannot write a closed file");
	size_t bytes_written = fwrite(buf, 1, n, ptr_);
	if (bytes_written != n) {
		// TODO: handle EINTR
		if (ferror(ptr_) != 0) {
			throw IOException(fmt::format("fwrite() failed for file {:?}: {}",
				path_.string(), errno_message()));
		} else {
			throw IOException(fmt::format("expected to write {} bytes but only wrote {} for file {:?}",
				n, bytes_written, path_.string()));
		}
	}
}
