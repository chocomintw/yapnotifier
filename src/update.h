#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Self-updater. Checks the latest GitHub release once per launch; if newer,
// downloads YapNotifier.asi, verifies it against GitHub's SHA-256 digest, and
// swaps it into place. Windows lets a mapped DLL be renamed, so the swap is
// immediate and the new file loads on the next FiveM start.
namespace yap::update {

struct Release {
    std::string tag;         // "v0.2.0"
    std::string asi_url;     // browser_download_url of the YapNotifier.asi asset
    std::string asi_sha256;  // lowercase hex from the asset's "digest"; empty if absent
};

// Pure helpers, exposed for tests.
bool parse_release(std::string_view json, Release& out);
bool is_newer(std::string_view tag, std::string_view current);  // "v0.2.0" > "0.1.0"
// Lines of CHANGELOG.md's "## <version>" section (blank lines dropped, "- " bullets stripped).
std::vector<std::string> changelog_section(std::string_view md, std::string_view version);

// Call at startup: removes the previous version left by a swap and sets the
// "updated" notice if one was found.
void cleanup_previous(const std::filesystem::path& self);

// Blocking network I/O; call from the init thread, never from Present.
void check_and_install(const std::filesystem::path& self, bool auto_install);

// One-line status for the overlay, or empty. Lock-free read.
std::shared_ptr<const std::string> notice();

}  // namespace yap::update
