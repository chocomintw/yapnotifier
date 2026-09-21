#include "ui_files.h"

#include <Windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace yap::ui_files {
namespace {

struct File {
    const Rml::byte* data;  // resource bytes, or null for a disk file
    size_t size, pos;
    FILE* disk;
};

HMODULE self_module() {
    static HMODULE self = [] {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&self_module), &m);
        return m;
    }();
    return self;
}

class ResourceFiles final : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override {
        const std::string name = resource_name(path);
        const std::wstring wname(name.begin(), name.end());
        if (auto span = resource(wname.c_str()); !span.empty())
            return reinterpret_cast<Rml::FileHandle>(new File{span.data(), span.size(), 0, nullptr});
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return 0;
        return reinterpret_cast<Rml::FileHandle>(new File{nullptr, 0, 0, f});
    }
    void Close(Rml::FileHandle h) override {
        auto* f = reinterpret_cast<File*>(h);
        if (f->disk) std::fclose(f->disk);
        delete f;
    }
    size_t Read(void* buf, size_t size, Rml::FileHandle h) override {
        auto* f = reinterpret_cast<File*>(h);
        if (f->disk) return std::fread(buf, 1, size, f->disk);
        const size_t n = size < f->size - f->pos ? size : f->size - f->pos;
        std::memcpy(buf, f->data + f->pos, n);
        f->pos += n;
        return n;
    }
    bool Seek(Rml::FileHandle h, long offset, int origin) override {
        auto* f = reinterpret_cast<File*>(h);
        if (f->disk) return std::fseek(f->disk, offset, origin) == 0;
        const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? static_cast<long>(f->pos) : static_cast<long>(f->size);
        const long to = base + offset;
        if (to < 0 || static_cast<size_t>(to) > f->size) return false;
        f->pos = static_cast<size_t>(to);
        return true;
    }
    size_t Tell(Rml::FileHandle h) override {
        auto* f = reinterpret_cast<File*>(h);
        return f->disk ? static_cast<size_t>(std::ftell(f->disk)) : f->pos;
    }
};

}  // namespace

Rml::String resource_name(const Rml::String& path) {
    std::string n = "UI_";
    for (unsigned char c : path) n += std::isalnum(c) ? static_cast<char>(std::toupper(c)) : '_';
    return n;
}

Rml::Span<const Rml::byte> resource(const wchar_t* name) {
    HMODULE self = self_module();
    HRSRC res = self ? FindResourceW(self, name, RT_RCDATA) : nullptr;
    HGLOBAL blob = res ? LoadResource(self, res) : nullptr;
    const void* data = blob ? LockResource(blob) : nullptr;
    const DWORD size = res ? SizeofResource(self, res) : 0;
    if (!data || !size) return {};
    return {static_cast<const Rml::byte*>(data), size};
}

Rml::FileInterface& instance() {
    static ResourceFiles files;
    return files;
}

}  // namespace yap::ui_files
