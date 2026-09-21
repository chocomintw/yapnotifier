#pragma once
// RmlUi file interface serving the UI documents (assets/ui/*) from RCDATA resources embedded in
// the .asi, so the single self-updating file stays self-contained. Any path with no matching
// resource falls through to the disk, which is how a user font_file gets loaded.
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Types.h>

namespace yap::ui_files {

// "icons/mic.svg" -> resource name "UI_ICONS_MIC_SVG" (see YapNotifier.rc).
Rml::String resource_name(const Rml::String& path);

// Bytes of an RCDATA resource in this module, or an empty span.
Rml::Span<const Rml::byte> resource(const wchar_t* name);

Rml::FileInterface& instance();

}  // namespace yap::ui_files
