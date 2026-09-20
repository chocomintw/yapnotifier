// SPDX-License-Identifier: MIT
#include "ts_contacts.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace tsro {
namespace {

bool same_key(std::string_view a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// The key a contact's identity is stored under.
///
/// Several spellings are accepted because the one this started with -- IDENTITY, upper case --
/// was a guess, and a wrong guess here produces no contacts at all rather than an error anyone
/// would notice.
bool is_identity_key(std::string_view key) {
    return same_key(key, "IDENTITY") || same_key(key, "UID") ||
           same_key(key, "UniqueIdentifier") || same_key(key, "Unique_Identifier") ||
           same_key(key, "ClientUniqueIdentifier");
}

/// Splits "Key=Value" out of one line, tolerating CR line endings and surrounding blanks.
bool split_setting(std::string_view line, std::string_view& key, std::string_view& value) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) return false;
    key = line.substr(0, eq);
    value = line.substr(eq + 1);
    return !key.empty();
}

/// TeamSpeak stores the contact state as a small integer, in the order its own Contacts dialog
/// lists the three options: Neutral, Blocked, Friend.
///
/// This was guessed the other way round first, which made every real friend read as blocked and
/// nobody read as a friend at all. The overlay only ever *reads* this value, so the cost of the
/// wrong guess was a missing colour rather than damage -- but it is a guess no longer: the
/// Diagnostics panel prints the raw number per person, and user_list.teamspeak_friend_value
/// overrides the mapping for a client that ever renumbers them.
ContactKind kind_from(std::string_view value) {
    if (value == "2") return ContactKind::Friend;
    if (value == "1") return ContactKind::Blocked;
    return ContactKind::Neutral;
}

}  // namespace

std::vector<Contact> parse_contacts(const std::vector<sqlite::Row>& rows) {
    std::vector<Contact> out;
    out.reserve(rows.size());

    for (const sqlite::Row& row : rows) {
        for (const std::string& column : row) {
            // No pre-filter on the column's contents: the blob is recognised by parsing it and
            // finding an identity, not by matching a spelling that was only ever assumed.
            if (column.find('=') == std::string::npos) continue;

            Contact contact;
            bool has_flag = false;
            std::size_t at = 0;
            while (at <= column.size()) {
                const std::size_t nl = column.find('\n', at);
                const std::string_view line(column.data() + at,
                                            (nl == std::string::npos ? column.size() : nl) - at);
                at = nl == std::string::npos ? column.size() + 1 : nl + 1;

                std::string_view key;
                std::string_view value;
                if (!split_setting(line, key, value)) continue;
                if (is_identity_key(key)) {
                    contact.unique_id.assign(value);
                } else if (same_key(key, "Nickname")) {
                    contact.nickname.assign(value);
                } else if (same_key(key, "Friend")) {
                    contact.kind = kind_from(value);
                    contact.raw_flag = 0;
                    for (const char c : value) {
                        if (c < '0' || c > '9') {
                            contact.raw_flag = -1;
                            break;
                        }
                        contact.raw_flag = contact.raw_flag * 10 + (c - '0');
                    }
                    if (value.empty()) contact.raw_flag = -1;
                    has_flag = true;
                }
            }
            (void)has_flag;
            if (contact.unique_id.empty()) continue;   // not the column carrying the contact
            out.push_back(std::move(contact));
            break;   // one identity per row
        }
    }

    // Later rows win, so a duplicated identity resolves to the most recent entry.
    std::stable_sort(out.begin(), out.end(),
                     [](const Contact& l, const Contact& r) { return l.unique_id < r.unique_id; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Contact& l, const Contact& r) {
                              return l.unique_id == r.unique_id;
                          }),
              out.end());
    return out;
}

std::string ContactReadReport::summary() const {
    std::string s = std::to_string(parsed) + " contacts";
    if (!table.empty()) s += " from table '" + table + "'";
    s += " (" + std::to_string(rows) + " rows, " + std::to_string(blobs) + " candidate values, " +
         std::to_string(tables) + " tables in " + path + ")";
    return s;
}

std::vector<std::string> contact_blobs(const std::vector<sqlite::Row>& rows) {
    std::vector<std::string> out;
    for (const sqlite::Row& row : rows) {
        for (const std::string& column : row) {
            // Long enough to hold an identity, and carrying at least one key=value pair. That is
            // all that is assumed about it.
            if (column.size() < 24 || column.find('=') == std::string::npos) continue;
            out.push_back(column);
        }
    }
    return out;
}

bool contact_from_blob(const std::string& blob, const std::string& unique_id, Contact& out) {
    if (unique_id.empty() || blob.find(unique_id) == std::string::npos) return false;
    out = Contact{};
    out.unique_id = unique_id;

    std::size_t at = 0;
    while (at <= blob.size()) {
        const std::size_t nl = blob.find('\n', at);
        const std::string_view line(blob.data() + at,
                                    (nl == std::string::npos ? blob.size() : nl) - at);
        at = nl == std::string::npos ? blob.size() + 1 : nl + 1;
        std::string_view key;
        std::string_view value;
        if (!split_setting(line, key, value)) continue;
        if (same_key(key, "Nickname")) {
            out.nickname.assign(value);
        } else if (same_key(key, "Friend")) {
            out.kind = kind_from(value);
            out.raw_flag = 0;
            for (const char ch : value) {
                if (ch < '0' || ch > '9') {
                    out.raw_flag = -1;
                    break;
                }
                out.raw_flag = out.raw_flag * 10 + (ch - '0');
            }
            if (value.empty()) out.raw_flag = -1;
        }
    }
    return true;
}

bool read_contacts(const std::string& config_dir, std::vector<Contact>& out, std::string& error,
                   ContactReadReport* report, std::vector<std::string>* blobs_out) {
    out.clear();
    error.clear();
    ContactReadReport local;
    ContactReadReport& r = report != nullptr ? *report : local;
    r = ContactReadReport{};

    if (config_dir.empty()) {
        error = "TeamSpeak did not report a configuration folder";
        return false;
    }
    std::string path = config_dir;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    path += "settings.db";
    r.path = path;

    std::vector<std::string> tables;
    std::string list_error;
    sqlite::list_tables(path, tables, list_error);
    r.tables = tables.size();

    // The expected name first, then everything else. Searching costs one pass over a settings
    // file; assuming the name and being wrong costs the entire feature, silently.
    std::vector<std::string> order;
    order.push_back("Contacts");
    for (const std::string& name : tables) {
        if (name != "Contacts") order.push_back(name);
    }

    // Everything contact-shaped from every table, kept whatever the structured parse makes of
    // it. Identity matching against these is what survives a field this code has never seen.
    std::vector<std::string> blobs;
    for (const std::string& name : order) {
        std::vector<sqlite::Row> rows;
        std::string read_error;
        if (!sqlite::read_table(path, name, rows, read_error)) {
            if (error.empty()) error = read_error;
            continue;
        }
        if (rows.empty()) continue;

        std::vector<std::string> from_table = contact_blobs(rows);
        blobs.insert(blobs.end(), std::make_move_iterator(from_table.begin()),
                     std::make_move_iterator(from_table.end()));

        std::vector<Contact> parsed = parse_contacts(rows);
        if (!parsed.empty() && out.empty()) {
            r.table = name;
            r.rows = rows.size();
            r.parsed = parsed.size();
            out = std::move(parsed);
            error.clear();
        }
    }
    r.blobs = blobs.size();
    if (blobs_out != nullptr) *blobs_out = std::move(blobs);
    if (!out.empty()) return true;

    // Nothing found is not a failure -- somebody with no contacts is an ordinary case -- but the
    // report says where we looked, which is what tells the two apart.
    if (!list_error.empty() && error.empty()) error = list_error;
    return error.empty();
}

}  // namespace tsro
