#include "update.h"

#include "log.h"
#include "version.h"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <charconv>
#include <fstream>

namespace {
using namespace yap;

std::atomic<std::shared_ptr<const std::string>> g_notice{std::make_shared<const std::string>()};

void set_notice(std::string s) { g_notice.store(std::make_shared<const std::string>(std::move(s))); }

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);  // URLs/paths we log are ASCII
    return s;
}

// --- tiny JSON-ish helper (GitHub emits compact "key":"value" pairs) ---------------
std::string quoted_after(std::string_view s, std::string_view key, size_t from = 0,
                         size_t until = std::string_view::npos) {
    size_t k = s.find(key, from);
    if (k == std::string_view::npos || k >= until) return {};
    size_t q1 = s.find('"', k + key.size());
    if (q1 == std::string_view::npos) return {};
    size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string_view::npos) return {};
    return std::string(s.substr(q1 + 1, q2 - q1 - 1));
}

// --- HTTP ---------------------------------------------------------------------------------
struct HInternet {
    HINTERNET h = nullptr;
    ~HInternet() {
        if (h) WinHttpCloseHandle(h);
    }
};

// GET an https URL; body appended to `out`. Follows redirects (GitHub asset
// downloads bounce to objects.githubusercontent.com). Caps at `max_bytes`.
bool http_get(const std::wstring& url, std::string& out, size_t max_bytes) {
    wchar_t host[256], path[2048];
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;

    HInternet session{WinHttpOpen(L"YapNotifier/" YAP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return false;
    WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 15000);
    HInternet conn{WinHttpConnect(session.h, host, uc.nPort, 0)};
    if (!conn.h) return false;
    HInternet req{WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!req.h) return false;
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        log::error("update: request failed for {} (err {})", narrow(url), GetLastError());
        return false;
    }
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        log::error("update: HTTP {} for {}", status, narrow(url));
        return false;
    }
    char buf[16384];
    DWORD got = 0;
    while (WinHttpReadData(req.h, buf, sizeof buf, &got) && got) {
        out.append(buf, got);
        if (out.size() > max_bytes) return false;
    }
    return true;
}

// --- SHA-256 (CNG) ---------------------------------------------------------------------
std::string sha256_hex(std::string_view data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32];
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                       static_cast<ULONG>(data.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
        static constexpr char kHex[] = "0123456789abcdef";
        for (unsigned char b : digest) {
            hex += kHex[b >> 4];
            hex += kHex[b & 15];
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// --- install ------------------------------------------------------------------------------
bool swap_in(const std::filesystem::path& self, const std::string& bytes) {
    const std::wstring download = self.wstring() + L".download";
    const std::wstring old = self.wstring() + L".old";
    {
        std::ofstream f(download, std::ios::binary | std::ios::trunc);
        if (!f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
            log::error("update: cannot write {}", narrow(download));
            return false;
        }
    }
    DeleteFileW(old.c_str());
    if (!MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        log::error("update: rename current .asi to .old failed (err {})", GetLastError());
        DeleteFileW(download.c_str());
        return false;
    }
    if (!MoveFileExW(download.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        log::error("update: move .download into place failed (err {}); rolling back", GetLastError());
        MoveFileExW(old.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(download.c_str());
        return false;
    }
    return true;
}
}  // namespace

namespace yap::update {

bool parse_release(std::string_view json, Release& out) {
    out = {};
    out.tag = quoted_after(json, "\"tag_name\":");
    if (out.tag.empty()) return false;

    size_t asset = json.find("\"name\":\"YapNotifier.asi\"");
    if (asset == std::string_view::npos) return true;  // release exists but has no .asi asset
    // GitHub emits browser_download_url last in each asset object, so it bounds this asset.
    size_t end = json.find("\"browser_download_url\":", asset);
    out.asi_url = quoted_after(json, "\"browser_download_url\":", asset);
    std::string digest = quoted_after(json, "\"digest\":", asset, end);
    if (digest.starts_with("sha256:") && digest.size() == 7 + 64 &&
        digest.find_first_not_of("0123456789abcdef", 7) == std::string::npos) {
        out.asi_sha256 = digest.substr(7);
    }
    return true;
}

bool is_newer(std::string_view tag, std::string_view current) {
    auto parse = [](std::string_view v, int (&n)[3]) {
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
        for (int& part : n) part = 0;
        for (int& part : n) {
            auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), part);
            if (ec != std::errc{}) return false;
            v.remove_prefix(static_cast<size_t>(p - v.data()));
            if (v.empty()) return true;
            if (v[0] != '.') return false;
            v.remove_prefix(1);
        }
        return true;
    };
    int a[3], b[3];
    if (!parse(tag, a) || !parse(current, b)) return false;
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

std::vector<std::string> changelog_section(std::string_view md, std::string_view version) {
    std::vector<std::string> out;
    bool in = false;
    while (!md.empty()) {
        const size_t nl = md.find('\n');
        std::string_view line = md.substr(0, nl);
        md = nl == std::string_view::npos ? std::string_view{} : md.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("## ")) {
            if (in) break;
            std::string_view rest = line.substr(3);
            in = rest.starts_with(version) && (rest.size() == version.size() || rest[version.size()] == ' ');
            continue;
        }
        if (!in || line.find_first_not_of(' ') == std::string_view::npos) continue;
        if (line.starts_with("- ")) line.remove_prefix(2);
        out.emplace_back(line);
    }
    return out;
}

void cleanup_previous(const std::filesystem::path& self) {
    const std::wstring old = self.wstring() + L".old";
    if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (DeleteFileW(old.c_str())) {
        log::info("update: running v" YAP_VERSION " after update; removed previous version");
        set_notice("YapNotifier updated to v" YAP_VERSION);
    } else {
        log::error("update: could not delete {} (err {})", narrow(old), GetLastError());
    }
}

void check_and_install(const std::filesystem::path& self, bool auto_install) {
    std::string json;
    if (!http_get(L"https://api.github.com/repos/" YAP_GITHUB_REPO "/releases/latest", json, 1u << 20)) {
        log::info("update: check skipped (offline, GitHub unreachable, or no release yet)");
        return;
    }
    Release rel;
    if (!parse_release(json, rel)) {
        log::error("update: could not parse release JSON");
        return;
    }
    if (!is_newer(rel.tag, YAP_VERSION)) {
        log::info("update: v" YAP_VERSION " is current (latest {})", rel.tag);
        return;
    }
    log::info("update: {} available (running v" YAP_VERSION ")", rel.tag);
    if (!auto_install) {
        set_notice("YapNotifier " + rel.tag + " is available (auto_update=0) - github.com/" YAP_GITHUB_REPO);
        return;
    }
    if (rel.asi_url.empty() || rel.asi_sha256.empty()) {
        log::error("update: release {} has no YapNotifier.asi asset with a digest; not installing", rel.tag);
        set_notice("YapNotifier " + rel.tag + " is available but cannot be verified - update manually");
        return;
    }

    std::string bytes;
    if (!http_get(std::wstring(rel.asi_url.begin(), rel.asi_url.end()), bytes, 32u << 20)) {
        log::error("update: download failed");
        return;
    }
    if (bytes.size() < 1024 || bytes[0] != 'M' || bytes[1] != 'Z') {
        log::error("update: downloaded file is not a PE image; discarded");
        return;
    }
    if (std::string got = sha256_hex(bytes); got != rel.asi_sha256) {
        log::error("update: SHA-256 mismatch (got {}, expected {}); discarded", got, rel.asi_sha256);
        return;
    }
    if (!swap_in(self, bytes)) return;
    log::info("update: {} installed; takes effect on next FiveM start", rel.tag);
    set_notice("YapNotifier " + rel.tag + " downloaded - restart FiveM to apply");
}

std::shared_ptr<const std::string> notice() { return g_notice.load(); }

}  // namespace yap::update
