#include "KitStock.h"
#include "PersistentFile.h"

#include "EngineContext.h"
#include "PathManager.h"
#include "common/io/FileWriter.h"

#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace KitStock {

namespace {

struct Cursor {
	const std::vector<uint8_t>& data;
	size_t pos = 0;
	bool ok = true;
	bool have(size_t n) {
		ok = ok && pos <= data.size() && n <= data.size() - pos;
		return ok;
	}
	uint32_t u16() {
		if (!have(2)) {
			return 0;
		}
		uint32_t v = data[pos] | (data[pos + 1] << 8);
		pos += 2;
		return v;
	}
	uint32_t u32() {
		if (!have(4)) {
			return 0;
		}
		uint32_t v = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
		pos += 4;
		return v;
	}
	std::string text() {
		if (!have(1)) {
			return "";
		}
		size_t len = data[pos++];
		if (!have(len)) {
			return "";
		}
		std::string s(data.begin() + static_cast<long>(pos), data.begin() + static_cast<long>(pos + len));
		pos += len;
		return s;
	}
};

void put16(std::vector<uint8_t>& out, uint32_t v) {
	out.push_back(static_cast<uint8_t>(v));
	out.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
	put16(out, v & 0xffff);
	put16(out, v >> 16);
}

void put_text(std::vector<uint8_t>& out, const std::string& s) {
	out.push_back(static_cast<uint8_t>(std::min<size_t>(s.size(), 255)));
	out.insert(out.end(), s.begin(), s.begin() + static_cast<long>(std::min<size_t>(s.size(), 255)));
}

} // namespace

std::vector<Item> load(const std::string& file) {
	std::vector<Item> items;
	auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, file);
	if (path.empty()) {
		return items;
	}
	std::ifstream in(path, std::ios::binary);
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	Cursor c{data};
	const uint32_t count = c.u16();
	for (uint32_t i = 0; i < count && c.ok; ++i) {
		Item item;
		item.stock = static_cast<uint16_t>(c.u16());
		item.script = c.text();
		item.width = static_cast<int32_t>(c.u32());
		item.height = static_cast<int32_t>(c.u32());
		item.row_length = static_cast<uint16_t>(c.u16());
        if (item.width <= 0 || item.height <= 0
            || static_cast<size_t>(item.width) > data.size() / static_cast<size_t>(item.height)) break;
		const size_t size = static_cast<size_t>(item.width) * static_cast<size_t>(item.height);
		if (!c.have(size)) {
			break;
		}
		item.pixels.assign(data.begin() + static_cast<long>(c.pos), data.begin() + static_cast<long>(c.pos + size));
		c.pos += size;
		item.name = c.text();
		item.description = c.text();
		if (c.ok) {
			items.push_back(std::move(item));
		}
	}
	return items;
}

void save(const std::string& file, const std::vector<Item>& items) {
	std::vector<uint8_t> out;
	put16(out, static_cast<uint32_t>(items.size()));
	for (const auto& item : items) {
		put16(out, item.stock);
		put_text(out, item.script);
		put32(out, static_cast<uint32_t>(item.width));
		put32(out, static_cast<uint32_t>(item.height));
		put16(out, item.row_length);
		out.insert(out.end(), item.pixels.begin(), item.pixels.end());
		put_text(out, item.name);
		put_text(out, item.description);
	}
    auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "") / file;
    write_persistent_file(path, out.data(), out.size());
}

bool replenish(const std::string& file) {
	auto backup = g_engine_context.paths->find_path(PATH_TYPE_MAIN, file + "BU");
	if (backup.empty()) {
		return false;
	}
	std::ifstream in(backup, std::ios::binary);
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "") / file;
    write_persistent_file(path, data.data(), data.size());
	return true;
}

} // namespace KitStock
