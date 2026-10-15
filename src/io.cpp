// SPDX-License-Identifier: see LICENSE
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "ipbp/ipbp.hpp"
#include "util.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ipbp {
namespace {

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
FILE* openFile(const std::string& p, const char* mode) {
    std::wstring wm(mode, mode + strlen(mode));
    return _wfopen(widen(p).c_str(), wm.c_str());
}
int seek64(FILE* f, uint64_t off) { return _fseeki64(f, (long long)off, SEEK_SET); }
uint64_t size64(FILE* f) {
    _fseeki64(f, 0, SEEK_END);
    return (uint64_t)_ftelli64(f);
}
#else
FILE* openFile(const std::string& p, const char* mode) { return fopen(p.c_str(), mode); }
int seek64(FILE* f, uint64_t off) { return fseeko(f, (off_t)off, SEEK_SET); }
uint64_t size64(FILE* f) {
    fseeko(f, 0, SEEK_END);
    return (uint64_t)ftello(f);
}
#endif

class FileInput : public InputStream {
public:
    explicit FileInput(FILE* f) : f_(f) {
        size_ = size64(f_);
        pos_ = UINT64_MAX;
    }
    ~FileInput() override { fclose(f_); }
    uint64_t size() const override { return size_; }
    bool readAt(uint64_t offset, void* buf, size_t len) override {
        if (offset + len > size_) return false;
        if (pos_ != offset && seek64(f_, offset) != 0) return false;
        size_t got = fread(buf, 1, len, f_);
        pos_ = offset + got;
        if (got != len) pos_ = UINT64_MAX;
        return got == len;
    }

private:
    FILE* f_;
    uint64_t size_, pos_;
};

class FileOutput : public OutputStream {
public:
    explicit FileOutput(FILE* f) : f_(f), pos_(0) {}
    ~FileOutput() override {
        if (f_) fclose(f_);
    }
    bool writeAt(uint64_t offset, const void* buf, size_t len) override {
        if (pos_ != offset && seek64(f_, offset) != 0) return false;
        size_t put = fwrite(buf, 1, len, f_);
        pos_ = offset + put;
        return put == len;
    }
    bool finish(uint64_t finalSize) override {
        if (fflush(f_) != 0) return false;
#ifdef _WIN32
        if (_chsize_s(_fileno(f_), (long long)finalSize) != 0) return false;
#else
        if (ftruncate(fileno(f_), (off_t)finalSize) != 0) return false;
#endif
        bool ok = fclose(f_) == 0;
        f_ = nullptr;
        return ok;
    }

private:
    FILE* f_;
    uint64_t pos_;
};

}  // namespace

std::unique_ptr<InputStream> openInputFile(const std::string& path, std::string* error) {
    FILE* f = openFile(path, "rb");
    if (!f) {
        if (error) *error = "cannot open '" + path + "': " + strerror(errno);
        return nullptr;
    }
    return std::unique_ptr<InputStream>(new FileInput(f));
}

std::unique_ptr<OutputStream> openOutputFile(const std::string& path, std::string* error) {
    FILE* f = openFile(path, "wb");
    if (!f) {
        if (error) *error = "cannot create '" + path + "': " + strerror(errno);
        return nullptr;
    }
    return std::unique_ptr<OutputStream>(new FileOutput(f));
}

namespace detail {

bool removeFile(const std::string& path) {
#ifdef _WIN32
    return DeleteFileW(widen(path).c_str()) != 0;
#else
    return unlink(path.c_str()) == 0;
#endif
}

bool renameReplace(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}

}  // namespace detail
}  // namespace ipbp
