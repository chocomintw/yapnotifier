// SPDX-License-Identifier: MIT
#include "sqlite_read.hpp"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace tsro::sqlite {
namespace {

constexpr char kMagic[] = "SQLite format 3";
constexpr std::size_t kHeaderSize = 100;
constexpr std::size_t kMaxFileBytes = 256u * 1024u * 1024u;

/// SQL compares identifiers without regard to case, so a table written as "contacts" must still
/// be found when asked for as "Contacts". Assuming otherwise is how this came back empty.
bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char l = static_cast<unsigned char>(a[i]);
        const unsigned char r = static_cast<unsigned char>(b[i]);
        if (std::tolower(l) != std::tolower(r)) return false;
    }
    return true;
}

std::uint16_t be16(const unsigned char* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(p[0]) << 8) | p[1]);
}

std::uint32_t be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

/// SQLite's big-endian base-128 varint: up to nine bytes, the ninth contributing all eight bits.
/// Returns the number of bytes consumed, or 0 if the buffer runs out.
std::size_t varint(const unsigned char* p, std::size_t available, std::uint64_t& out) {
    out = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        if (i >= available) return 0;
        out = (out << 7) | static_cast<std::uint64_t>(p[i] & 0x7F);
        if ((p[i] & 0x80) == 0) return i + 1;
    }
    if (available < 9) return 0;
    out = (out << 8) | static_cast<std::uint64_t>(p[8]);
    return 9;
}

bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxFileBytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.resize(static_cast<std::size_t>(size));
    if (out.empty()) return true;
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(in);
}

/// A database image plus whatever the write-ahead log has superseded in it.
class Pager {
public:
    bool init(const std::vector<unsigned char>* db, const std::vector<unsigned char>* wal,
              std::string& error) {
        db_ = db;
        if (db_->size() < kHeaderSize ||
            std::memcmp(db_->data(), kMagic, sizeof(kMagic)) != 0) {
            error = "not a SQLite database";
            return false;
        }
        const std::uint16_t declared = be16(db_->data() + 16);
        page_size_ = declared == 1 ? 65536u : static_cast<std::uint32_t>(declared);
        if (page_size_ < 512 || (page_size_ & (page_size_ - 1)) != 0) {
            error = "unsupported page size";
            return false;
        }
        reserved_ = (*db_)[20];
        if (reserved_ >= page_size_) {
            error = "unsupported reserved space";
            return false;
        }
        if (wal != nullptr && !wal->empty()) index_wal(*wal);
        return true;
    }

    std::uint32_t page_size() const noexcept { return page_size_; }
    std::uint32_t usable_size() const noexcept { return page_size_ - reserved_; }

    /// Page numbers are 1-based. Returns nullptr when the page is outside the image.
    const unsigned char* page(std::uint32_t number) const {
        if (number == 0) return nullptr;
        const auto from_wal = wal_pages_.find(number);
        if (from_wal != wal_pages_.end()) return wal_->data() + from_wal->second;
        const std::size_t offset =
            static_cast<std::size_t>(number - 1) * static_cast<std::size_t>(page_size_);
        if (offset + page_size_ > db_->size()) return nullptr;
        return db_->data() + offset;
    }

private:
    /// Indexes the write-ahead log so the newest image of each page wins.
    ///
    /// Only frames belonging to the current salt are considered, and only those up to the last
    /// commit frame -- everything after it is a transaction that was never committed and must
    /// not be read. Checksums are not verified: this is a reader, not a recovery tool, and a
    /// corrupt frame would fail to parse as a page anyway.
    void index_wal(const std::vector<unsigned char>& wal) {
        constexpr std::size_t kWalHeader = 32;
        constexpr std::size_t kFrameHeader = 24;
        if (wal.size() < kWalHeader) return;
        const std::uint32_t magic = be32(wal.data());
        if (magic != 0x377F0682u && magic != 0x377F0683u) return;
        const std::uint32_t wal_page_size = be32(wal.data() + 8);
        if (wal_page_size != page_size_) return;
        const std::uint32_t salt1 = be32(wal.data() + 16);
        const std::uint32_t salt2 = be32(wal.data() + 20);

        std::map<std::uint32_t, std::size_t> pending;
        std::size_t offset = kWalHeader;
        while (offset + kFrameHeader + page_size_ <= wal.size()) {
            const unsigned char* frame = wal.data() + offset;
            if (be32(frame + 8) != salt1 || be32(frame + 12) != salt2) break;
            const std::uint32_t page_number = be32(frame);
            const std::uint32_t commit_size = be32(frame + 4);
            pending[page_number] = offset + kFrameHeader;
            if (commit_size != 0) {
                // A commit frame makes every frame up to here durable.
                for (const auto& [number, where] : pending) wal_pages_[number] = where;
                pending.clear();
            }
            offset += kFrameHeader + page_size_;
        }
        if (!wal_pages_.empty()) wal_ = &wal;
    }

    const std::vector<unsigned char>* db_ = nullptr;
    const std::vector<unsigned char>* wal_ = nullptr;
    std::map<std::uint32_t, std::size_t> wal_pages_;
    std::uint32_t page_size_ = 0;
    std::uint32_t reserved_ = 0;
};

/// Reassembles a cell payload, following the overflow chain when it does not fit on the page.
bool payload_bytes(const Pager& pager, const unsigned char* local, std::uint64_t total,
                   std::size_t local_available, std::vector<unsigned char>& out) {
    const std::uint32_t usable = pager.usable_size();
    const std::uint32_t max_local = usable - 35;
    out.clear();
    if (total > 64u * 1024u * 1024u) return false;

    if (total <= max_local) {
        if (total > local_available) return false;
        out.assign(local, local + static_cast<std::size_t>(total));
        return true;
    }

    // The spill formula from the file format: enough stays on the page that a record's header
    // is nearly always local, which is why this is not simply "fill the page".
    const std::uint32_t min_local = ((usable - 12) * 32 / 255) - 23;
    const std::uint64_t k = min_local + ((total - min_local) % (usable - 4));
    const std::uint32_t on_page =
        k <= max_local ? static_cast<std::uint32_t>(k) : min_local;
    if (on_page + 4u > local_available) return false;

    out.reserve(static_cast<std::size_t>(total));
    out.insert(out.end(), local, local + on_page);
    std::uint32_t next = be32(local + on_page);

    std::set<std::uint32_t> seen;
    while (out.size() < total) {
        if (next == 0 || !seen.insert(next).second) return false;   // truncated or a cycle
        const unsigned char* page = pager.page(next);
        if (page == nullptr) return false;
        const std::size_t want =
            std::min<std::size_t>(usable - 4, static_cast<std::size_t>(total) - out.size());
        out.insert(out.end(), page + 4, page + 4 + want);
        next = be32(page);
    }
    return true;
}

std::string format_int(std::int64_t v) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(v));
    return buffer;
}

std::string format_real(double v) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%g", v);
    return buffer;
}

/// Decodes one record into text columns.
bool decode_record(const std::vector<unsigned char>& payload, Row& out) {
    out.clear();
    std::uint64_t header_size = 0;
    const std::size_t header_varint = varint(payload.data(), payload.size(), header_size);
    if (header_varint == 0 || header_size > payload.size()) return false;

    std::vector<std::uint64_t> types;
    std::size_t at = header_varint;
    while (at < header_size) {
        std::uint64_t type = 0;
        const std::size_t used = varint(payload.data() + at, header_size - at, type);
        if (used == 0) return false;
        types.push_back(type);
        at += used;
    }

    std::size_t body = static_cast<std::size_t>(header_size);
    for (std::uint64_t type : types) {
        const auto take = [&](std::size_t n) -> const unsigned char* {
            if (body + n > payload.size()) return nullptr;
            const unsigned char* p = payload.data() + body;
            body += n;
            return p;
        };
        if (type == 0) {
            out.emplace_back();
        } else if (type >= 1 && type <= 6) {
            static constexpr std::size_t kWidths[] = {0, 1, 2, 3, 4, 6, 8};
            const std::size_t width = kWidths[type];
            const unsigned char* p = take(width);
            if (p == nullptr) return false;
            std::int64_t value = (p[0] & 0x80) ? -1 : 0;   // sign-extend
            for (std::size_t i = 0; i < width; ++i) {
                value = (value << 8) | static_cast<std::int64_t>(p[i]);
            }
            out.push_back(format_int(value));
        } else if (type == 7) {
            const unsigned char* p = take(8);
            if (p == nullptr) return false;
            std::uint64_t bits = 0;
            for (std::size_t i = 0; i < 8; ++i) bits = (bits << 8) | p[i];
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            out.push_back(format_real(value));
        } else if (type == 8) {
            out.emplace_back("0");
        } else if (type == 9) {
            out.emplace_back("1");
        } else if (type >= 12) {
            const std::size_t length = static_cast<std::size_t>((type - (type % 2 ? 13 : 12)) / 2);
            const unsigned char* p = take(length);
            if (p == nullptr) return false;
            out.emplace_back(reinterpret_cast<const char*>(p), length);
        } else {
            return false;   // 10 and 11 are reserved
        }
    }
    return true;
}

/// Walks a table b-tree, decoding every leaf cell.
bool walk_table(const Pager& pager, std::uint32_t root, std::vector<Row>& out,
                std::set<std::uint32_t>& visited, int depth) {
    if (depth > 32 || !visited.insert(root).second) return false;
    const unsigned char* page = pager.page(root);
    if (page == nullptr) return false;

    // Page 1 carries the 100-byte database header before its b-tree header.
    const std::size_t base = root == 1 ? kHeaderSize : 0;
    const unsigned char* header = page + base;
    const unsigned char type = header[0];
    const std::uint16_t cells = be16(header + 3);
    const std::size_t header_len = (type == 0x02 || type == 0x05) ? 12 : 8;
    const std::size_t pointer_array = base + header_len;
    const std::uint32_t usable = pager.usable_size();

    if (type != 0x0D && type != 0x05) return false;   // not a table b-tree

    for (std::uint16_t i = 0; i < cells; ++i) {
        const std::size_t slot = pointer_array + static_cast<std::size_t>(i) * 2u;
        if (slot + 2 > usable) return false;
        const std::uint16_t offset = be16(page + slot);
        if (offset == 0 || offset >= usable) return false;
        const unsigned char* cell = page + offset;
        const std::size_t available = usable - offset;

        if (type == 0x05) {
            if (available < 4) return false;
            const std::uint32_t child = be32(cell);
            walk_table(pager, child, out, visited, depth + 1);
            continue;
        }

        std::uint64_t payload_size = 0;
        std::size_t used = varint(cell, available, payload_size);
        if (used == 0) return false;
        std::uint64_t rowid = 0;
        const std::size_t rowid_used = varint(cell + used, available - used, rowid);
        if (rowid_used == 0) return false;
        used += rowid_used;

        std::vector<unsigned char> payload;
        if (!payload_bytes(pager, cell + used, payload_size, available - used, payload)) {
            continue;   // one unreadable row must not lose the rest of the table
        }
        Row row;
        if (decode_record(payload, row)) out.push_back(std::move(row));
    }

    if (type == 0x05) {
        // The rightmost child lives in the b-tree header, not the cell array.
        const std::uint32_t right = be32(header + 8);
        if (right != 0) walk_table(pager, right, out, visited, depth + 1);
    }
    return true;
}

}  // namespace

bool read_table_from_memory(const std::vector<unsigned char>& db,
                            const std::vector<unsigned char>& wal, const std::string& table,
                            std::vector<Row>& out, std::string& error) {
    out.clear();
    error.clear();
    Pager pager;
    if (!pager.init(&db, &wal, error)) return false;

    // sqlite_master is always the table b-tree rooted at page 1. Its columns are
    // (type, name, tbl_name, rootpage, sql).
    std::vector<Row> schema;
    std::set<std::uint32_t> visited;
    if (!walk_table(pager, 1, schema, visited, 0)) {
        error = "the schema table could not be read";
        return false;
    }

    std::uint32_t root = 0;
    for (const Row& row : schema) {
        if (row.size() < 4) continue;
        if (row[0] != "table" || !same_name(row[1], table)) continue;
        root = static_cast<std::uint32_t>(std::strtoul(row[3].c_str(), nullptr, 10));
        break;
    }
    if (root == 0) return true;   // no such table: no rows, not an error

    std::set<std::uint32_t> table_visited;
    if (!walk_table(pager, root, out, table_visited, 0)) {
        error = "table '" + table + "' could not be read";
        return false;
    }
    return true;
}

bool list_tables(const std::string& path, std::vector<std::string>& out, std::string& error) {
    out.clear();
    error.clear();
    std::vector<unsigned char> db;
    if (!read_file(path, db)) {
        error = "cannot read '" + path + "'";
        return false;
    }
    std::vector<unsigned char> wal;
    read_file(path + "-wal", wal);

    Pager pager;
    if (!pager.init(&db, &wal, error)) return false;
    std::vector<Row> schema;
    std::set<std::uint32_t> visited;
    if (!walk_table(pager, 1, schema, visited, 0)) {
        error = "the schema table could not be read";
        return false;
    }
    for (const Row& row : schema) {
        if (row.size() >= 2 && row[0] == "table") out.push_back(row[1]);
    }
    return true;
}

bool read_table(const std::string& path, const std::string& table, std::vector<Row>& out,
                std::string& error) {
    out.clear();
    error.clear();
    std::vector<unsigned char> db;
    if (!read_file(path, db)) {
        error = "cannot read '" + path + "'";
        return false;
    }
    std::vector<unsigned char> wal;
    read_file(path + "-wal", wal);   // absent is normal
    return read_table_from_memory(db, wal, table, out, error);
}

}  // namespace tsro::sqlite
