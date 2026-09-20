// SPDX-License-Identifier: MIT
// A read-only reader for a single SQLite table.
//
// TeamSpeak's friend list is not in the plugin API -- the whole SDK has no contact, friend or
// buddy call, which is verifiable by grepping it. It lives in the client's own settings.db, so
// that is where this reads it from.
//
// Deliberately minimal and deliberately read-only: it opens the file, never writes, never takes
// a lock and never runs SQL. It understands table b-trees, overflow pages and the write-ahead
// log, because a running TeamSpeak keeps recent changes in the -wal file and a reader that
// ignored it would report a stale list. Anything it does not understand is reported as an error
// rather than guessed at.
#ifndef TSRO_SQLITE_READ_HPP
#define TSRO_SQLITE_READ_HPP

#include <string>
#include <vector>

namespace tsro::sqlite {

/// One row. Every column is rendered as text: TEXT and BLOB come through as their bytes,
/// INTEGER and REAL are formatted, NULL is empty. That is all the callers here need, and it
/// keeps the decoder free of a variant type.
using Row = std::vector<std::string>;

/// Reads every row of `table`. The name is matched without regard to case, as SQL itself does.
///
/// Returns false with `error` set when the file is missing, is not a SQLite database, or uses a
/// feature this reader does not implement. A missing table is not an error: it yields no rows.
bool read_table(const std::string& path, const std::string& table, std::vector<Row>& out,
                std::string& error);

/// Names every table in the database. Used to find the contact list when it is not where it was
/// expected, and to say what *was* there when it cannot be found at all.
bool list_tables(const std::string& path, std::vector<std::string>& out, std::string& error);

/// Same, over a database image already in memory. `wal` may be empty. Exposed for tests.
bool read_table_from_memory(const std::vector<unsigned char>& db,
                            const std::vector<unsigned char>& wal, const std::string& table,
                            std::vector<Row>& out, std::string& error);

}  // namespace tsro::sqlite

#endif  // TSRO_SQLITE_READ_HPP
