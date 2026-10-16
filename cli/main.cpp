// SPDX-License-Identifier: see LICENSE
// ipbp - command line front-end for the iPBP library.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ipbp/ipbp.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
// shellapi.h must come after windows.h
#include <shellapi.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

ipbp::CancelToken g_cancel;
volatile std::sig_atomic_t g_signalled = 0;

void onSignal(int) {
    g_signalled = 1;
    g_cancel.cancel();
}

#ifdef _WIN32
BOOL WINAPI onConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        onSignal(0);
        return TRUE;
    }
    return FALSE;
}
#endif

void usage() {
    fprintf(stderr,
            "iPBP %d.%d.%d - PSP ISO <-> EBOOT.PBP (NPUMDIMG) converter\n"
            "\n"
            "Usage:\n"
            "  ipbp [options] <input.iso|input.pbp> [output]   convert (direction auto-detected)\n"
            "  ipbp iso2pbp [options] <input.iso> [EBOOT.PBP]\n"
            "  ipbp pbp2iso [options] <EBOOT.PBP> [output.iso]\n"
            "  ipbp info <EBOOT.PBP>\n"
            "  ipbp check <input.iso>                         good/over/underdump check\n"
            "\n"
            "If output is omitted (or is a directory) the name follows the original GUI:\n"
            "  ISO -> <input name>.PBP (or EBOOT.PBP inside a directory), PBP -> <DISC-ID>.ISO\n"
            "\n"
            "ISO -> PBP options:\n"
            "  -n, --no-compress        store blocks uncompressed\n"
            "  -m, --max                smallest LZRC (much slower; still decodes on a PSP,\n"
            "                           but not byte-identical to sign_np)\n"
            "  -c, --content-id ID      content id XXYYYY-AAAABBBBB_CC-DDDDDDDDDDDDDDDD\n"
            "                           (default UP0000-<DISC_ID>_00-0000000000000000)\n"
            "  -k, --version-key HEX    16-byte version key as 32 hex digits (default: fixed key)\n"
            "  -s, --startdat FILE.png  boot screen image (480x272 PNG)\n"
            "      --seed N             reproducible output (fixed header key / nonces)\n"
            "PBP -> ISO options:\n"
            "      --no-verify          skip per-block MAC verification\n"
            "      --trim               cut the image at the ISO9660 volume size\n"
            "Common:\n"
            "  -t, --threads N          worker threads (default: CPU count)\n"
            "  -f, --force              overwrite an existing output file\n"
            "  -q, --quiet              no progress / info output\n"
            "  -h, --help\n"
            "\n"
            "Press Ctrl+C to cancel; the partial output file is removed immediately.\n",
            ipbp::kVersionMajor, ipbp::kVersionMinor, ipbp::kVersionPatch);
}

bool fileExists(const std::string& p) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, &w[0], n);
    return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}

bool isDirectory(const std::string& p) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, &w[0], n);
    DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string dirName(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}
std::string stem(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    std::string f = s == std::string::npos ? p : p.substr(s + 1);
    size_t d = f.find_last_of('.');
    return d == std::string::npos || d == 0 ? f : f.substr(0, d);
}
std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    return (last == '/' || last == '\\') ? dir + name : dir + "/" + name;
}

bool readWhole(const std::string& path, std::vector<uint8_t>& out) {
    std::string err;
    auto in = ipbp::openInputFile(path, &err);
    if (!in) return false;
    out.resize((size_t)in->size());
    return out.empty() || in->readAt(0, out.data(), out.size());
}

bool parseHexKey(const std::string& s, std::array<uint8_t, 16>& k) {
    if (s.size() != 32) return false;
    for (int i = 0; i < 16; i++) {
        unsigned v;
        if (sscanf(s.c_str() + 2 * i, "%2x", &v) != 1) return false;
        k[i] = (uint8_t)v;
    }
    return true;
}

enum class Kind { Unknown, Iso, Pbp };

Kind detect(const std::string& path) {
    std::string err;
    auto in = ipbp::openInputFile(path, &err);
    if (!in) return Kind::Unknown;
    uint8_t m[8];
    if (in->size() >= 4 && in->readAt(0, m, 4) && memcmp(m, "\0PBP", 4) == 0) return Kind::Pbp;
    if (in->size() >= 0x8006 && in->readAt(0x8000, m, 6) && memcmp(m + 1, "CD001", 5) == 0) return Kind::Iso;
    return Kind::Unknown;
}

class ProgressPrinter {
public:
    explicit ProgressPrinter(bool enabled) : enabled_(enabled), tty_(isatty(fileno(stderr)) != 0) {
        start_ = std::chrono::steady_clock::now();
    }
    void update(uint64_t done, uint64_t total) {
        if (!enabled_) return;
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - start_).count();
        int pct = total ? (int)(done * 100 / total) : 100;
        if (!tty_) {  // log-friendly: print every 10%
            if (pct / 10 != lastPct_ / 10 || done == total) fprintf(stderr, "%d%%\n", pct);
            lastPct_ = pct;
            return;
        }
        if (done != total && std::chrono::duration<double>(now - lastDraw_).count() < 0.1) return;
        lastDraw_ = now;
        double mbps = secs > 0 ? done / secs / (1024.0 * 1024.0) : 0;
        int bar = total ? (int)(done * 30 / total) : 30;
        char b[31];
        for (int i = 0; i < 30; i++) b[i] = i < bar ? '#' : '.';
        b[30] = 0;
        fprintf(stderr, "\r[%s] %3d%%  %.1f MB/s ", b, pct, mbps);
        if (done == total) fputc('\n', stderr);
        fflush(stderr);
    }
    void clearLine() {
        if (enabled_ && tty_) fputc('\n', stderr);
    }

private:
    bool enabled_, tty_;
    int lastPct_ = -1;
    std::chrono::steady_clock::time_point start_, lastDraw_{};
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
    SetConsoleOutputCP(CP_UTF8);
#else
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#endif
#ifdef _WIN32
    // argv is in the ANSI code page on Windows; rebuild it from the UTF-16 command line so
    // file names in any script (e.g. Japanese titles) survive. The library takes UTF-8.
    std::vector<std::string> args;
    {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        for (int i = 1; wargv && i < wargc; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
            args.push_back(s);
        }
        if (wargv) LocalFree(wargv);
        (void)argc;
        (void)argv;
    }
#else
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    std::string mode;

    ipbp::IsoToPbpOptions iopt;
    ipbp::PbpToIsoOptions popt;
    bool force = false, quiet = false;
    unsigned threads = 0;
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= args.size()) {
                fprintf(stderr, "error: %s needs a value\n", what);
                exit(2);
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "-n" || a == "--no-compress") iopt.compress = false;
        else if (a == "-m" || a == "--max") iopt.maxCompression = true;
        else if (a == "-c" || a == "--content-id") iopt.contentId = need("--content-id");
        else if (a == "-k" || a == "--version-key") {
            std::array<uint8_t, 16> k;
            if (!parseHexKey(need("--version-key"), k)) {
                fprintf(stderr, "error: version key must be 32 hex digits\n");
                return 2;
            }
            iopt.versionKey = k;
        } else if (a == "-s" || a == "--startdat") {
            std::string p = need("--startdat");
            if (!readWhole(p, iopt.startdatPng) || iopt.startdatPng.size() < 8 ||
                memcmp(iopt.startdatPng.data(), "\x89PNG", 4) != 0) {
                fprintf(stderr, "error: '%s' is not a readable PNG file\n", p.c_str());
                return 2;
            }
        } else if (a == "--seed") iopt.deterministicSeed = strtoull(need("--seed").c_str(), nullptr, 0);
        else if (a == "--no-verify") popt.verify = false;
        else if (a == "--trim") popt.trimToVolumeSize = true;
        else if (a == "-t" || a == "--threads") threads = (unsigned)atoi(need("--threads").c_str());
        else if (a == "-f" || a == "--force") force = true;
        else if (a == "-q" || a == "--quiet") quiet = true;
        else if (a.size() > 1 && a[0] == '-') {
            fprintf(stderr, "error: unknown option %s (see --help)\n", a.c_str());
            return 2;
        } else if (pos.empty() && mode.empty() && (a == "iso2pbp" || a == "pbp2iso" || a == "info" || a == "check")) {
            mode = a;
        } else pos.push_back(a);
    }
    iopt.threads = popt.threads = threads;
    if (pos.empty() || pos.size() > 2) {
        usage();
        return 2;
    }
    const std::string input = pos[0];
    if (!fileExists(input)) {
        fprintf(stderr, "error: '%s' does not exist\n", input.c_str());
        return 2;
    }

    Kind kind = detect(input);
    if (mode == "info") {
        ipbp::PbpInfo info;
        ipbp::Result r = ipbp::readPbpInfo(input, info);
        if (!r) {
            fprintf(stderr, "error: %s\n", r.message.c_str());
            return 1;
        }
        char key[33];
        for (int i = 0; i < 16; i++) snprintf(key + 2 * i, 3, "%02X", info.versionKey[i]);
        printf("Title       : %s\nDisc ID     : %s\nContent ID  : %s\nNP flags    : 0x%08X\n"
               "Version key : %s\nBlocks      : %u x %u sectors%s\nISO size    : %llu bytes\n",
               info.title.c_str(), info.discId.c_str(), info.contentId.c_str(), info.npFlags, key, info.blocks,
               info.blockBasis, info.compressed ? " (LZRC compressed)" : "", (unsigned long long)info.isoSize);
        return 0;
    }
    if (mode == "check") {
        ipbp::IsoDumpCheck c;
        ipbp::Result r = ipbp::checkIsoDump(input, c);
        if (!r) {
            fprintf(stderr, "error: %s\n", r.message.c_str());
            return 1;
        }
        printf("File size       : %llu bytes\nCalculated size : %llu bytes\n", (unsigned long long)c.fileSize,
               (unsigned long long)c.volumeSize);
        if (c.good()) printf("Good dump!\n");
        else printf("Bad dump! %lld bytes %s\n", (long long)std::llabs(c.difference()),
                    c.difference() > 0 ? "overdump" : "underdump");
        return c.good() ? 0 : 3;
    }
    if (mode.empty()) {
        if (kind == Kind::Iso) mode = "iso2pbp";
        else if (kind == Kind::Pbp) mode = "pbp2iso";
        else {
            fprintf(stderr, "error: cannot tell whether '%s' is an ISO or a PBP\n", input.c_str());
            return 2;
        }
    }

    // Output naming
    std::string output = pos.size() > 1 ? pos[1] : std::string();
    if (mode == "iso2pbp") {
        if (output.empty()) output = dirName(input) + stem(input) + ".PBP";
        else if (isDirectory(output)) output = joinPath(output, "EBOOT.PBP");
    } else {
        std::string name;
        ipbp::PbpInfo info;
        if (ipbp::readPbpInfo(input, info)) name = ipbp::suggestedIsoName(info);
        if (name.empty() || name == "image.ISO") name = stem(input) + ".ISO";
        if (output.empty()) output = dirName(input) + name;
        else if (isDirectory(output)) output = joinPath(output, name);
    }
    if (!force && fileExists(output)) {
        fprintf(stderr, "error: '%s' already exists (use --force to overwrite)\n", output.c_str());
        return 2;
    }

    ProgressPrinter progress(!quiet);
    ipbp::Callbacks cb;
    cb.cancel = g_cancel;
    cb.progress = [&](uint64_t d, uint64_t t) { progress.update(d, t); };
    if (!quiet) cb.log = [](const std::string& s) { fprintf(stderr, "%s\n", s.c_str()); };

    if (!quiet) fprintf(stderr, "%s -> %s\n", input.c_str(), output.c_str());
    auto t0 = std::chrono::steady_clock::now();
    ipbp::Result r = mode == "iso2pbp" ? ipbp::isoToPbp(input, output, iopt, cb) : ipbp::pbpToIso(input, output, popt, cb);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!r) {
        if (r.status == ipbp::Status::Cancelled) {
            progress.clearLine();
            fprintf(stderr, "Cancelled.\n");
            return 130;
        }
        progress.clearLine();
        fprintf(stderr, "error: %s\n", r.message.c_str());
        return 1;
    }
    if (!quiet) fprintf(stderr, "Done in %.1fs.\n", secs);
    return 0;
}
