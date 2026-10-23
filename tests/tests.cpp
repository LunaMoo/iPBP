// SPDX-License-Identifier: see LICENSE
//
// Self-contained tests: synthetic PSP-style ISO images are built in memory, converted to
// PBP and back, and the result is checked bit-for-bit. No game data is needed.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "crypto/ecdsa.h"
#include "crypto/sha1.h"
#include "ipbp/ipbp.h"
#include "ipbp/ipbp.hpp"
#include "psp/lzrc.h"
#include "util.h"

using namespace ipbp;
using detail::rd32;
using detail::wr16;
using detail::wr32;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                        \
    } while (0)

// ------------------------------------------------------------------ memory streams

struct MemIn : InputStream {
    const std::vector<uint8_t>& d;
    explicit MemIn(const std::vector<uint8_t>& v) : d(v) {}
    uint64_t size() const override { return d.size(); }
    bool readAt(uint64_t off, void* buf, size_t len) override {
        if (off + len > d.size()) return false;
        memcpy(buf, d.data() + off, len);
        return true;
    }
};

struct MemOut : OutputStream {
    std::vector<uint8_t> d;
    bool writeAt(uint64_t off, const void* buf, size_t len) override {
        if (off + len > d.size()) d.resize((size_t)(off + len));
        memcpy(d.data() + off, buf, len);
        return true;
    }
    bool finish(uint64_t n) override {
        d.resize((size_t)n);
        return true;
    }
};

// ------------------------------------------------------------------ synthetic ISO

static std::vector<uint8_t> makeSfo(const std::string& discId, const std::string& title) {
    struct E {
        std::string k, v;
        uint32_t max;
    };
    std::vector<E> es = {{"CATEGORY", "UG", 4},
                         {"DISC_ID", discId, 16},
                         {"PSP_SYSTEM_VER", "6.20", 8},
                         {"TITLE", title, 128}};  // sorted by key
    std::vector<uint8_t> idx, keys, data;
    for (auto& e : es) {
        uint8_t r[16];
        wr16(r, (uint16_t)keys.size());
        wr16(r + 2, 0x0204);
        wr32(r + 4, (uint32_t)e.v.size() + 1);
        wr32(r + 8, e.max);
        wr32(r + 12, (uint32_t)data.size());
        idx.insert(idx.end(), r, r + 16);
        keys.insert(keys.end(), e.k.begin(), e.k.end());
        keys.push_back(0);
        size_t at = data.size();
        data.resize(at + e.max, 0);
        memcpy(&data[at], e.v.data(), e.v.size());
    }
    while (keys.size() % 4) keys.push_back(0);
    std::vector<uint8_t> out(20, 0);
    memcpy(&out[0], "\0PSF", 4);
    wr32(&out[4], 0x0101);
    wr32(&out[8], (uint32_t)(20 + idx.size()));
    wr32(&out[12], (uint32_t)(20 + idx.size() + keys.size()));
    wr32(&out[16], (uint32_t)es.size());
    out.insert(out.end(), idx.begin(), idx.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

static void both32(uint8_t* p, uint32_t v) {
    wr32(p, v);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

static size_t dirRec(uint8_t* p, const std::string& name, uint32_t lba, uint32_t size, bool dir) {
    size_t nlen = name.empty() ? 1 : name.size();
    size_t len = 33 + nlen;
    if (len & 1) len++;
    memset(p, 0, len);
    p[0] = (uint8_t)len;
    both32(p + 2, lba);
    both32(p + 10, size);
    p[25] = dir ? 2 : 0;
    p[28] = 1;
    p[32] = (uint8_t)nlen;
    if (name.empty()) p[33] = 0;
    else if (name == "\x01") p[33] = 1;
    else memcpy(p + 33, name.data(), nlen);
    return len;
}

// Builds a minimal ISO9660 image with /PSP_GAME/PARAM.SFO and `sectors` sectors in total.
// Content is a mix of zeros, repetitive text and random noise so that both the compressed and
// the stored (incompressible) code paths are exercised.
static std::vector<uint8_t> makeIso(uint32_t sectors, const std::string& discId = "ULUS99999",
                                    uint32_t seed = 1, uint64_t extraBytes = 0) {
    const uint32_t S = 2048;
    std::vector<uint8_t> iso((size_t)sectors * S + extraBytes, 0);
    std::vector<uint8_t> sfo = makeSfo(discId, "iPBP Test Disc");
    const uint32_t rootLba = 18, gameLba = 19, sfoLba = 20, umdLba = 21;
    // PVD
    uint8_t* pvd = &iso[16 * S];
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + 40, "IPBP_TEST", 9);
    both32(pvd + 80, sectors);
    dirRec(pvd + 156, "", rootLba, S, true);
    // terminator
    iso[17 * S] = 255;
    memcpy(&iso[17 * S + 1], "CD001", 5);
    // root
    uint8_t* r = &iso[rootLba * S];
    r += dirRec(r, "", rootLba, S, true);
    r += dirRec(r, "\x01", rootLba, S, true);
    r += dirRec(r, "PSP_GAME", gameLba, S, true);
    dirRec(r, "UMD_DATA.BIN;1", umdLba, 10, false);
    // PSP_GAME
    uint8_t* g = &iso[gameLba * S];
    g += dirRec(g, "", gameLba, S, true);
    g += dirRec(g, "\x01", rootLba, S, true);
    dirRec(g, "PARAM.SFO;1", sfoLba, (uint32_t)sfo.size(), false);
    memcpy(&iso[sfoLba * S], sfo.data(), sfo.size());
    std::string umd = discId.substr(0, 4) + "-" + discId.substr(4);
    memcpy(&iso[umdLba * S], umd.data(), umd.size());
    // payload
    std::mt19937 rng(seed);
    const char* text = "The quick brown fox jumps over the lazy PSP. ";
    for (uint32_t s = 24; s < sectors; s++) {
        uint8_t* p = &iso[(size_t)s * S];
        switch ((s / 7) % 4) {
            case 0: break;  // zeros
            case 1:
                for (uint32_t i = 0; i < S; i++) p[i] = (uint8_t)text[(i + s) % 45];
                break;
            case 2:
                for (uint32_t i = 0; i < S; i++) p[i] = (uint8_t)rng();
                break;
            case 3:
                for (uint32_t i = 0; i < S; i++) p[i] = (uint8_t)(i * s);
                break;
        }
    }
    return iso;
}

// ------------------------------------------------------------------ helpers

static const uint8_t kSignPriv[20] = {0x14, 0xB0, 0x22, 0xE8, 0x92, 0xCF, 0x86, 0x14, 0xA4, 0x45,
                                      0x57, 0xDB, 0x09, 0x5C, 0x92, 0x8D, 0xE9, 0xB8, 0x99, 0x70};

static std::string tmpPath(const char* name) {
    static int n = 0;
    return "ipbp_test_" + std::to_string(++n) + "_" + name;
}
static bool exists(const std::string& p) { return std::ifstream(p, std::ios::binary).good(); }
static void writeFile(const std::string& p, const std::vector<uint8_t>& d) {
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
static std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool roundTrip(const std::vector<uint8_t>& iso, const IsoToPbpOptions& o, std::vector<uint8_t>* pbpOut = nullptr) {
    MemIn in(iso);
    MemOut pbp;
    Result r = isoToPbp(in, pbp, o);
    if (!r) {
        fprintf(stderr, "  isoToPbp: %s\n", r.message.c_str());
        return false;
    }
    MemIn pin(pbp.d);
    MemOut back;
    r = pbpToIso(pin, back);
    if (!r) {
        fprintf(stderr, "  pbpToIso: %s\n", r.message.c_str());
        return false;
    }
    if (pbpOut) *pbpOut = pbp.d;
    // Output is padded to whole sectors; anything beyond the input must be zero.
    if (back.d.size() < iso.size()) return false;
    if (memcmp(back.d.data(), iso.data(), iso.size()) != 0) return false;
    for (size_t i = iso.size(); i < back.d.size(); i++)
        if (back.d[i]) return false;
    return true;
}

// ------------------------------------------------------------------ tests

static void testRoundTrips() {
    puts("round trips");
    auto iso = makeIso(700);  // 43.75 blocks: last block partial
    IsoToPbpOptions o;
    CHECK(roundTrip(iso, o));
    o.compress = false;
    CHECK(roundTrip(iso, o));
    o.compress = true;
    o.threads = 1;
    CHECK(roundTrip(iso, o));
    o.threads = 7;
    CHECK(roundTrip(iso, o));
    // max compression: same round trip, never larger than the default
    {
        IsoToPbpOptions d, mx;
        d.deterministicSeed = mx.deterministicSeed = 42;
        mx.maxCompression = true;
        std::vector<uint8_t> pd, pm;
        CHECK(roundTrip(iso, d, &pd));
        CHECK(roundTrip(iso, mx, &pm));
        CHECK(pm.size() <= pd.size());
        mx.threads = 3;
        CHECK(roundTrip(makeIso(16 * 40, "ULES00001", 5), mx));
    }
    // exactly whole blocks
    CHECK(roundTrip(makeIso(16 * 40, "ULES00001", 5), IsoToPbpOptions{}));
    // tiny image (single block)
    CHECK(roundTrip(makeIso(30, "NPJH00002", 6), IsoToPbpOptions{}));
    // non sector-aligned input size
    CHECK(roundTrip(makeIso(100, "ULUS10041", 7, 1000), IsoToPbpOptions{}));
}

// Both LZRC encoders on blocks built to hit the format's edges: 2-3 byte matches (distance must
// be < 256), long runs (match length cap), distances near the window limit, random data, and
// every block size up to a full 32 KiB block. Each result must decode back exactly, and max
// must never be larger.
static void testLzrcEncoders() {
    puts("LZRC encoders");
    std::mt19937 rng(1234);
    int bad = 0, larger = 0, smaller = 0;
    std::vector<uint8_t> in(0x8000), o1(0x10000), o2(0x10000), back(0x8000);
    for (int t = 0; t < 400; t++) {
        int n = t < 100 ? 1 + (int)(rng() % 0x8000) : 0x8000;
        for (int i = 0; i < n;) {
            int kind = rng() % 6, len = 1 + rng() % 600;
            for (int k = 0; k < len && i < n; k++, i++) {
                switch (kind) {
                    case 0: in[i] = (uint8_t)rng(); break;                                     // random
                    case 1: in[i] = 0; break;                                                  // run
                    case 2: in[i] = i >= 3 ? in[i - 2 - rng() % 2] : 7; break;                 // tiny repeats
                    case 3: in[i] = i >= 300 ? in[i - 300 + (rng() % 4 == 0)] : 3; break;     // mid distance
                    case 4: in[i] = i >= 0x7F00 ? in[i - 0x7F00] : (uint8_t)(i * 7); break;   // far distance
                    default: in[i] = (uint8_t)("PSP game data "[i % 14] ^ (rng() % 16 == 0)); break;
                }
            }
        }
        int c1 = ipbp::psp::lzrcCompress(o1.data(), (int)o1.size() - 16, in.data(), n);
        int c2 = ipbp::psp::lzrcCompressMax(o2.data(), (int)o2.size() - 16, in.data(), n);
        if (c1 <= 0 || c2 <= 0) {
            bad++;
            continue;
        }
        for (auto [o, c] : {std::pair<uint8_t*, int>{o1.data(), c1}, {o2.data(), c2}}) {
            int d = ipbp::psp::lzrcDecompress(back.data(), n, o, c);
            if (d != n || memcmp(back.data(), in.data(), n) != 0) bad++;
        }
        if (c2 > c1) larger++;
        if (c2 < c1) smaller++;
    }
    CHECK(bad == 0);
    CHECK(larger == 0);
    CHECK(smaller > 200);
    printf("  max smaller on %d/400 blocks\n", smaller);
}

static void testKeysAndIds() {
    puts("content id / version key");
    auto iso = makeIso(200);
    IsoToPbpOptions o;
    o.contentId = "EP9000-NPEZ00123_00-ABCDEFGHIJKLMNOP";
    std::array<uint8_t, 16> key;
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i * 17 + 3);
    o.versionKey = key;
    std::vector<uint8_t> pbp;
    CHECK(roundTrip(iso, o, &pbp));
    MemIn in(pbp);
    PbpInfo info;
    CHECK(readPbpInfo(in, info).ok());
    CHECK(info.contentId == o.contentId);
    CHECK(info.discId == "NPEZ-00123");
    CHECK(info.versionKey == key);
    CHECK(info.npFlags == 0x00000002);
    CHECK(info.title == "iPBP Test Disc");
    CHECK(suggestedIsoName(info) == "NPEZ-00123.ISO");

    // default content id + fixed key
    IsoToPbpOptions d;
    CHECK(roundTrip(iso, d, &pbp));
    MemIn in2(pbp);
    CHECK(readPbpInfo(in2, info).ok());
    CHECK(info.contentId == "UP0000-ULUS99999_00-0000000000000000");
    CHECK(info.npFlags == 0x01000003);
    CHECK(info.compressed);
    CHECK(info.blocks == (200 + 15) / 16);

    // invalid content id rejected
    IsoToPbpOptions bad;
    bad.contentId = "nonsense";
    MemIn in3(iso);
    MemOut out;
    CHECK(isoToPbp(in3, out, bad).status == Status::InvalidInput);
}

static void testSignatures() {
    puts("ECDSA signatures");
    auto iso = makeIso(120);
    std::vector<uint8_t> pbp;
    CHECK(roundTrip(iso, IsoToPbpOptions{}, &pbp));
    uint8_t pub[40];
    ecdsa::publicKey(kSignPriv, pub);
    // DATA.PSP: sig over SHA1(PARAM.SFO || DATA.PSP[0x560..0x590])
    uint32_t sfoOff = rd32(&pbp[8]), sfoEnd = rd32(&pbp[12]), dpsp = rd32(&pbp[0x20]), psar = rd32(&pbp[0x24]);
    Sha1 h;
    h.update(&pbp[sfoOff], sfoEnd - sfoOff);
    h.update(&pbp[dpsp + 0x560], 0x30);
    uint8_t dg[20];
    h.finish(dg);
    CHECK(ecdsa::verify(pub, dg, &pbp[dpsp]));
    // NPUMDIMG header: sig over SHA1(header[0..0xD8])
    Sha1::hash(&pbp[psar], 0xD8, dg);
    CHECK(ecdsa::verify(pub, dg, &pbp[psar + 0xD8]));
    dg[0] ^= 1;
    CHECK(!ecdsa::verify(pub, dg, &pbp[psar + 0xD8]));
    // PARAM.SFO was patched to CATEGORY "EG"
    std::string sfo((const char*)&pbp[sfoOff], sfoEnd - sfoOff);
    CHECK(sfo.find("EG") != std::string::npos);
}

static void testDeterministic() {
    puts("reproducible output");
    auto iso = makeIso(150);
    IsoToPbpOptions o;
    o.deterministicSeed = 42;
    std::vector<uint8_t> a, b, c;
    CHECK(roundTrip(iso, o, &a));
    o.threads = 3;
    CHECK(roundTrip(iso, o, &b));
    CHECK(a == b);
    o.deterministicSeed = 43;
    CHECK(roundTrip(iso, o, &c));
    CHECK(a != c);
    // without a seed every run uses a fresh header key
    IsoToPbpOptions r;
    CHECK(roundTrip(iso, r, &a));
    CHECK(roundTrip(iso, r, &b));
    CHECK(a != b);
}

static void testTamper() {
    puts("corruption detection");
    auto iso = makeIso(300);
    std::vector<uint8_t> pbp;
    IsoToPbpOptions o;
    o.compress = false;
    CHECK(roundTrip(iso, o, &pbp));
    uint32_t psar = rd32(&pbp[0x24]);
    pbp[pbp.size() - 40000] ^= 0x40;  // flip a bit inside image data
    {
        MemIn in(pbp);
        MemOut out;
        Result r = pbpToIso(in, out);
        CHECK(r.status == Status::VerifyFailed);
    }
    {
        MemIn in(pbp);
        MemOut out;
        PbpToIsoOptions p;
        p.verify = false;
        CHECK(pbpToIso(in, out, p).ok());  // decrypts anyway, data differs
        CHECK(out.d.size() >= iso.size() && memcmp(out.d.data(), iso.data(), iso.size()) != 0);
    }
    {
        std::vector<uint8_t> t(pbp.begin(), pbp.begin() + psar + 0x100 + 0x40);  // truncated
        MemIn in(t);
        MemOut out;
        CHECK(pbpToIso(in, out).status == Status::InvalidInput);
    }
}

static void testBadInput() {
    puts("invalid input");
    std::vector<uint8_t> junk(100000, 0xAB);
    MemIn in(junk);
    MemOut out;
    CHECK(isoToPbp(in, out).status == Status::InvalidInput);
    CHECK(pbpToIso(in, out).status == Status::InvalidInput);
    // ISO9660 without PSP_GAME
    auto iso = makeIso(64);
    iso[19 * 2048 + 34 * 2 + 33] = 'X';  // rename PARAM.SFO record in PSP_GAME
    std::vector<uint8_t> bad = iso;
    for (size_t i = 19 * 2048; i < 20 * 2048 - 9; i++)
        if (!memcmp(&bad[i], "PARAM.SFO", 9)) bad[i] = 'Q';
    MemIn in2(bad);
    CHECK(isoToPbp(in2, out).status == Status::InvalidInput);
    // PS1 eboot
    std::vector<uint8_t> ps1(0x2000, 0);
    memcpy(&ps1[0], "\0PBP", 4);
    wr32(&ps1[0x24], 0x1000);
    memcpy(&ps1[0x1000], "PSISOIMG0000", 12);
    MemIn in3(ps1);
    CHECK(pbpToIso(in3, out).status == Status::Unsupported);
}

static void testCancelFromCallback() {
    puts("cancellation (from progress callback)");
    auto iso = makeIso(16 * 400);
    for (int dir = 0; dir < 2; dir++) {
        std::vector<uint8_t> pbp;
        if (dir == 1) CHECK(roundTrip(iso, IsoToPbpOptions{}, &pbp));
        Callbacks cb;
        int calls = 0;
        uint64_t lastDone = 0;
        cb.progress = [&](uint64_t done, uint64_t total) {
            lastDone = done;
            if (++calls == 3) cb.cancel.cancel();
            (void)total;
        };
        MemIn in(dir == 0 ? iso : pbp);
        MemOut out;
        Result r = dir == 0 ? isoToPbp(in, out, IsoToPbpOptions{}, cb) : pbpToIso(in, out, PbpToIsoOptions{}, cb);
        CHECK(r.status == Status::Cancelled);
        CHECK(lastDone < iso.size());
        CHECK(calls == 3);  // no progress after cancel
    }
}

static void testCancelFromThreadAndFiles() {
    puts("cancellation (other thread, file API)");
    auto iso = makeIso(16 * 3000);  // ~94 MiB
    std::string isoP = tmpPath("in.iso"), pbpP = tmpPath("out.pbp"), isoOut = tmpPath("out.iso");
    writeFile(isoP, iso);

    Callbacks cb;
    std::atomic<bool> started{false};
    cb.progress = [&](uint64_t done, uint64_t) {
        if (done > 0) started = true;
    };
    Result r;
    auto t0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point tCancel;
    std::thread worker([&] { r = isoToPbp(isoP, pbpP, IsoToPbpOptions{}, cb); });
    while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tCancel = std::chrono::steady_clock::now();
    cb.cancel.cancel();
    worker.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tCancel).count();
    printf("  iso->pbp stopped %lld ms after cancel (%lld ms total)\n", (long long)ms,
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tCancel - t0).count());
    CHECK(r.status == Status::Cancelled);
    CHECK(ms < 500);
    CHECK(!exists(pbpP));
    CHECK(!exists(pbpP + ".part"));

    // full conversion through files, then cancel the reverse direction
    cb.cancel.reset();
    started = false;
    CHECK(isoToPbp(isoP, pbpP, IsoToPbpOptions{}, Callbacks{}).ok());
    CHECK(exists(pbpP) && !exists(pbpP + ".part"));
    std::thread w2([&] { r = pbpToIso(pbpP, isoOut, PbpToIsoOptions{}, cb); });
    while (!started) std::this_thread::sleep_for(std::chrono::microseconds(100));
    cb.cancel.cancel();
    w2.join();
    CHECK(r.status == Status::Cancelled);
    CHECK(!exists(isoOut) && !exists(isoOut + ".part"));

    // and to completion
    CHECK(pbpToIso(pbpP, isoOut, PbpToIsoOptions{}, Callbacks{}).ok());
    CHECK(readFile(isoOut) == iso);
    // existing output is replaced atomically
    CHECK(pbpToIso(pbpP, isoOut, PbpToIsoOptions{}, Callbacks{}).ok());
    CHECK(readFile(isoOut) == iso);
    std::remove(isoP.c_str());
    std::remove(pbpP.c_str());
    std::remove(isoOut.c_str());
}

static void testCApi() {
    puts("C API");
    auto iso = makeIso(500, "ULJM05000");
    std::string isoP = tmpPath("c.iso"), pbpP = tmpPath("c.pbp"), outP = tmpPath("c_out.iso");
    writeFile(isoP, iso);
    ipbp_job* job = ipbp_job_create();
    struct Ctx {
        int calls = 0;
        uint64_t total = 0;
        ipbp_job* job = nullptr;
        bool cancelAt2 = false;
    } ctx;
    ctx.job = job;
    ipbp_job_set_progress(
        job,
        [](void* u, uint64_t, uint64_t total) {
            auto* c = (Ctx*)u;
            c->total = total;
            if (++c->calls == 2 && c->cancelAt2) ipbp_job_cancel(c->job);
        },
        &ctx);
    ipbp_iso2pbp_options io;
    ipbp_iso2pbp_options_init(&io);
    CHECK(io.compress == 1);
    CHECK(ipbp_iso_to_pbp(job, isoP.c_str(), pbpP.c_str(), &io) == IPBP_OK);
    CHECK(ctx.total == iso.size());
    ipbp_pbp_info info;
    CHECK(ipbp_read_pbp_info(job, pbpP.c_str(), &info) == IPBP_OK);
    CHECK(std::string(info.disc_id) == "ULJM-05000");
    CHECK(std::string(info.content_id) == "UP0000-ULJM05000_00-0000000000000000");
    CHECK(ipbp_pbp_to_iso(job, pbpP.c_str(), outP.c_str(), nullptr) == IPBP_OK);
    CHECK(readFile(outP) == iso);
    std::remove(outP.c_str());
    io.compress = IPBP_COMPRESS_MAX;
    CHECK(ipbp_iso_to_pbp(job, isoP.c_str(), pbpP.c_str(), &io) == IPBP_OK);
    CHECK(ipbp_pbp_to_iso(job, pbpP.c_str(), outP.c_str(), nullptr) == IPBP_OK);
    CHECK(readFile(outP) == iso);
    std::remove(outP.c_str());
    // cancel
    ctx.calls = 0;
    ctx.cancelAt2 = true;
    CHECK(ipbp_pbp_to_iso(job, pbpP.c_str(), outP.c_str(), nullptr) == IPBP_CANCELLED);
    CHECK(!exists(outP));
    CHECK(std::string(ipbp_job_error(job)).size() > 0);
    // errors
    CHECK(ipbp_pbp_to_iso(job, "does/not/exist.pbp", outP.c_str(), nullptr) == IPBP_IO_ERROR);
    CHECK(ipbp_iso_to_pbp(job, pbpP.c_str(), outP.c_str(), nullptr) == IPBP_INVALID_INPUT);
    CHECK(std::string(ipbp_version()).size() > 0);
    ipbp_job_destroy(job);
    std::remove(isoP.c_str());
    std::remove(pbpP.c_str());
}

// The callback variants: ISO from memory -> PBP file -> ISO into memory.
static void testCApiCallbacks() {
    puts("C API callbacks");
    auto iso = makeIso(500, "ULJM05001");
    std::string pbpP = tmpPath("cb.pbp");
    ipbp_job* job = ipbp_job_create();

    struct Src {
        const std::vector<uint8_t>* d;
    } src{&iso};
    ipbp_reader r;
    r.user = &src;
    r.size = iso.size();
    r.read_at = [](void* u, uint64_t off, void* buf, size_t len) -> int {
        const auto& d = *((Src*)u)->d;
        if (off + len > d.size()) return 1;
        memcpy(buf, d.data() + off, len);
        return 0;
    };
    CHECK(ipbp_iso_to_pbp_reader(job, &r, pbpP.c_str(), nullptr) == IPBP_OK);
    CHECK(!exists(pbpP + ".part"));

    struct Dst {
        std::vector<uint8_t> d;
        uint64_t begun = 0, finished = 0;
        bool refuse = false;
    };
    ipbp_writer w;
    w.begin = [](void* u, uint64_t size) -> int {
        auto* d = (Dst*)u;
        if (d->refuse) return 1;
        d->begun = size;
        d->d.assign((size_t)size, 0);
        return 0;
    };
    w.write_at = [](void* u, uint64_t off, const void* buf, size_t len) -> int {
        auto* d = (Dst*)u;
        if (off + len > d->d.size()) return 1;
        memcpy(d->d.data() + off, buf, len);
        return 0;
    };
    w.finish = [](void* u, uint64_t size) -> int {
        auto* d = (Dst*)u;
        d->finished = size;
        d->d.resize((size_t)size);
        return 0;
    };
    Dst dst;
    w.user = &dst;
    CHECK(ipbp_pbp_to_iso_writer(job, pbpP.c_str(), &w, nullptr) == IPBP_OK);
    CHECK(dst.begun >= dst.finished && dst.finished == iso.size());
    CHECK(dst.d == iso);

    // ISO in memory -> PBP in memory: the file's size (the header has random bytes), and it
    // converts back to the ISO
    {
        struct Grow {
            std::vector<uint8_t> d;
            uint64_t finished = 0;
        } g;
        ipbp_writer gw;
        gw.user = &g;
        gw.begin = nullptr;
        gw.write_at = [](void* u, uint64_t off, const void* buf, size_t len) -> int {
            auto* d = (Grow*)u;
            if (off + len > d->d.size()) d->d.resize((size_t)(off + len));
            memcpy(d->d.data() + off, buf, len);
            return 0;
        };
        gw.finish = [](void* u, uint64_t size) -> int {
            auto* d = (Grow*)u;
            d->finished = size;
            d->d.resize((size_t)size);
            return 0;
        };
        CHECK(ipbp_iso_to_pbp_io(job, &r, &gw, nullptr) == IPBP_OK);
        CHECK(g.finished == g.d.size());
        CHECK(g.d.size() == readFile(pbpP).size());
        std::string memP = tmpPath("cb_mem.pbp");
        if (FILE* f = fopen(memP.c_str(), "wb")) {
            fwrite(g.d.data(), 1, g.d.size(), f);
            fclose(f);
        }
        Dst back;
        w.user = &back;
        CHECK(ipbp_pbp_to_iso_writer(job, memP.c_str(), &w, nullptr) == IPBP_OK);
        CHECK(back.d == iso);
        std::remove(memP.c_str());
        CHECK(ipbp_iso_to_pbp_io(job, &r, nullptr, nullptr) == IPBP_INVALID_INPUT);
    }

    Dst refused;
    refused.refuse = true;
    w.user = &refused;
    CHECK(ipbp_pbp_to_iso_writer(job, pbpP.c_str(), &w, nullptr) == IPBP_IO_ERROR);
    CHECK(refused.d.empty());

    ipbp_job_destroy(job);
    std::remove(pbpP.c_str());
}

static void testDumpCheck() {
    puts("ISO dump check");
    IsoDumpCheck c;
    auto good = makeIso(100);
    MemIn a(good);
    CHECK(checkIsoDump(a, c).ok() && c.good() && c.volumeSize == 100 * 2048ull);
    auto over = makeIso(100, "ULUS99999", 1, 4096);
    MemIn b(over);
    CHECK(checkIsoDump(b, c).ok() && !c.good() && c.difference() == 4096);
    std::vector<uint8_t> under(good.begin(), good.end() - 2048);
    MemIn d(under);
    CHECK(checkIsoDump(d, c).ok() && c.difference() == -2048);
    // the converter warns about it
    Callbacks cb;
    bool warned = false;
    cb.log = [&](const std::string& l) { warned |= l.find("underdump") != std::string::npos; };
    MemOut out;
    MemIn d2(under);
    CHECK(isoToPbp(d2, out, IsoToPbpOptions{}, cb).ok());
    CHECK(warned);
    std::vector<uint8_t> junk(50000, 1);
    MemIn j(junk);
    CHECK(checkIsoDump(j, c).status == Status::InvalidInput);
    // C API
    std::string p = tmpPath("chk.iso");
    writeFile(p, over);
    ipbp_job* job = ipbp_job_create();
    uint64_t fs = 0, vs = 0;
    CHECK(ipbp_check_iso(job, p.c_str(), &fs, &vs) == IPBP_OK && fs == over.size() && vs == 100 * 2048ull);
    ipbp_job_destroy(job);
    std::remove(p.c_str());
}

// ------------------------------------------------------------------ PSOne EBOOTs

// A PlayStation disc: MODE2 form 1 raw sectors with ISO9660 and a SYSTEM.CNF, then `audioFrames`
// frames of "audio".
static std::vector<uint8_t> makePsxDisc(uint32_t dataSectors, const std::string& boot, uint32_t audioFrames, uint32_t seed) {
    const uint32_t S = 2048, R = 2352;
    std::vector<uint8_t> iso((size_t)dataSectors * S, 0);
    uint8_t* pvd = &iso[16 * S];
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    both32(pvd + 80, dataSectors);
    dirRec(pvd + 156, "", 18, S, true);
    iso[17 * S] = 255;
    memcpy(&iso[17 * S + 1], "CD001", 5);
    std::string cnf = "BOOT = " + boot + "\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n";
    uint8_t* r = &iso[18 * S];
    r += dirRec(r, "", 18, S, true);
    r += dirRec(r, "\x01", 18, S, true);
    dirRec(r, "SYSTEM.CNF;1", 19, (uint32_t)cnf.size(), false);
    memcpy(&iso[19 * S], cnf.data(), cnf.size());
    std::mt19937 rng(seed);
    for (uint32_t s = 24; s < dataSectors; s++)
        for (uint32_t i = 0; i < S; i++) iso[(size_t)s * S + i] = (s % 3) ? (uint8_t)rng() : (uint8_t)(i / 7);
    std::vector<uint8_t> raw((size_t)(dataSectors + audioFrames) * R, 0);
    auto b = [](uint32_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); };
    for (uint32_t s = 0; s < dataSectors; s++) {
        uint8_t* p = &raw[(size_t)s * R];
        memset(p + 1, 0xff, 10);
        uint32_t a = s + 150;
        p[12] = b(a / 4500);
        p[13] = b((a / 75) % 60);
        p[14] = b(a % 75);
        p[15] = 2;
        p[18] = p[22] = 8;  // form 1 data
        memcpy(p + 24, &iso[(size_t)s * S], S);
    }
    for (size_t i = (size_t)dataSectors * R; i < raw.size(); i++) raw[i] = (uint8_t)(rng() >> (i % 5));
    return raw;
}

static void testPsx() {
    printf("PSOne EBOOTs\n");
    {
        std::vector<uint8_t> probe(2352 * 20, 0);
        MemIn in(probe);
        PsxDisc d;
        d.image = &in;
        MemOut out;
        if (psxToPbp({d}, out).status == Status::Unsupported) {
            printf("  skipped: built without zlib\n");
            return;
        }
    }
    // One disc: a data track, and an audio track with a 2-second pregap in the image.
    std::vector<uint8_t> disc = makePsxDisc(300, "cdrom:\\SLUS_005.94;1", 400, 3);
    CHECK(disc.size() % 2352 == 0);
    {
        MemIn in(disc);
        CHECK(psxDiscId(in) == "SLUS00594");
    }
    std::vector<PsxTrack> tracks = {PsxTrack{false, 0, 0}, PsxTrack{true, 300, 450}};
    for (int level : {9, 1, 0}) {
        MemIn in(disc);
        PsxDisc d;
        d.image = &in;
        d.tracks = tracks;
        PsxToPbpOptions o;
        o.level = level;
        o.title = "iPBP PSOne Test";
        MemOut pbp;
        Result r = psxToPbp({d}, pbp, o);
        CHECK(r.ok());
        MemIn pin(pbp.d);
        CHECK(pbpKind(pin) == PbpKind::Psx);
        PsxInfo info;
        CHECK(readPsxInfo(pin, info).ok());
        CHECK(info.title == "iPBP PSOne Test" && info.discId == "SLUS00594" && !info.hasDataPsp);
        CHECK(info.discs.size() == 1);
        if (info.discs.size() == 1) {
            const PsxDiscInfo& di = info.discs[0];
            CHECK(di.discId == "SLUS00594" && di.frames == 700);
            CHECK(di.tracks.size() == 2);
            if (di.tracks.size() == 2) {
                CHECK(!di.tracks[0].audio && di.tracks[0].start == 0 && di.tracks[0].index1 == 0);
                CHECK(di.tracks[1].audio && di.tracks[1].start == 300 && di.tracks[1].index1 == 450);
                CHECK(di.dataModes[0] == 2 && di.dataModes[1] == 0);
            }
            if (level == 9) CHECK(di.storedBytes < disc.size());
            if (level == 0) CHECK(di.storedBytes == (uint64_t)di.blocks * 0x9300);
        }
        // The layout emulators rely on.
        uint32_t psar = rd32(&pbp.d[0x24]);
        CHECK(memcmp(&pbp.d[psar], "PSISOIMG0000", 12) == 0 && memcmp(&pbp.d[psar + 0x400], "_SLUS_00594", 11) == 0);
        CHECK(rd32(&pbp.d[psar + 0xBFC]) == 0x100000);
        CHECK(pbp.d[psar + 0x800 + 2] == 0xA0 && pbp.d[psar + 0x80A + 2] == 0xA1 && pbp.d[psar + 0x814 + 2] == 0xA2);
        MemOut back;
        CHECK(pbpToPsx(pin, 0, back).ok());
        CHECK(back.d == disc);
    }

    // Two discs in one EBOOT.
    std::vector<uint8_t> disc2 = makePsxDisc(200, "cdrom:\\SLUS_005.95;1", 0, 4);
    {
        MemIn a(disc), b2(disc2);
        PsxDisc d1, d2;
        d1.image = &a;
        d1.tracks = tracks;
        d2.image = &b2;
        MemOut pbp;
        CHECK(psxToPbp({d1, d2}, pbp, PsxToPbpOptions{}).ok());
        MemIn pin(pbp.d);
        PsxInfo info;
        CHECK(readPsxInfo(pin, info).ok());
        CHECK(info.discs.size() == 2);
        if (info.discs.size() == 2) {
            CHECK(info.discs[0].discId == "SLUS00594" && info.discs[1].discId == "SLUS00595");
            CHECK(info.discs[1].tracks.size() == 1 && info.discs[1].frames == 200);
        }
        uint32_t psar = rd32(&pbp.d[0x24]);
        CHECK(memcmp(&pbp.d[psar], "PSTITLEIMG000000", 16) == 0);
        MemOut o1, o2;
        CHECK(pbpToPsx(pin, 0, o1).ok() && o1.d == disc);
        CHECK(pbpToPsx(pin, 1, o2).ok() && o2.d == disc2);
        CHECK(!pbpToPsx(pin, 2, o2).ok());
    }

    // A base PBP: DATA.PSP and STARTDAT come from it, with our boot screen.
    {
        std::vector<uint8_t> base(0x50000 + 0x2000, 0);
        memcpy(&base[0], "\0PBP", 4);
        wr32(&base[4], 0x10000);
        std::vector<uint8_t> dataPsp(0x400, 0x5A);
        wr32(&dataPsp[0x2C], (uint32_t)dataPsp.size());
        uint32_t offs[8] = {0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x50000};
        offs[7] = 0x28 + (uint32_t)dataPsp.size();
        offs[7] = 0x10000;
        for (int i = 0; i < 8; i++) wr32(&base[8 + 4 * i], offs[i]);
        memcpy(&base[0x28], dataPsp.data(), dataPsp.size());
        memcpy(&base[0x10000], "PSISOIMG0000", 12);
        wr32(&base[0x1000C], 0x30000);  // STARTDAT at PSAR + 0x30000
        uint8_t* sd = &base[0x40000];
        memcpy(sd, "STARTDAT", 8);
        wr32(sd + 16, 0x50);
        wr32(sd + 20, 0x100);
        memset(sd + 0x50, 0xB0, 0x100);  // the base's boot PNG
        base.resize(0x40000 + 0x50 + 0x100);
        const char* pgd = "\0PGD and the rest";
        base.insert(base.end(), pgd, pgd + 17);
        std::string basePath = tmpPath("psx_base.pbp");
        writeFile(basePath, base);

        MemIn in(disc2);
        PsxDisc d;
        d.image = &in;
        PsxToPbpOptions o;
        o.basePbpPath = basePath;
        o.bootPng.assign(64, 0xC1);
        MemOut pbp;
        Result r = psxToPbp({d}, pbp, o);
        CHECK(r.ok());
        MemIn pin(pbp.d);
        PsxInfo info;
        CHECK(readPsxInfo(pin, info).ok() && info.hasDataPsp);
        uint32_t psp = rd32(&pbp.d[0x20]);
        CHECK(memcmp(&pbp.d[psp], dataPsp.data(), dataPsp.size()) == 0);
        uint32_t psar = rd32(&pbp.d[0x24]);
        uint32_t end = rd32(&pbp.d[psar + 0x0C]);
        CHECK(psar + end + 0x50 + 64 + 17 == pbp.d.size());
        if (psar + end + 8 <= pbp.d.size()) {
            CHECK(memcmp(&pbp.d[psar + end], "STARTDAT", 8) == 0 && rd32(&pbp.d[psar + end + 20]) == 64);
            CHECK(pbp.d[psar + end + 0x50] == 0xC1 && memcmp(&pbp.d[pbp.d.size() - 17], pgd, 17) == 0);
        }
        MemOut back;
        CHECK(pbpToPsx(pin, 0, back).ok() && back.d == disc2);
        // C API: kind, info and the image through a writer.
        std::string path = tmpPath("psx_eboot.pbp");
        writeFile(path, pbp.d);
        CHECK(ipbp_pbp_kind(path.c_str()) == IPBP_PBP_PSX);
        ipbp_job* job = ipbp_job_create();
        ipbp_psx_info ci;
        CHECK(ipbp_read_psx_info(job, path.c_str(), &ci) == IPBP_OK && ci.discs == 1 && ci.disc[0].frames == 200 && ci.has_data_psp);
        CHECK(strcmp(ci.disc[0].disc_id, "SLUS00595") == 0);
        std::vector<uint8_t> got;
        ipbp_writer w = {&got, [](void* u, uint64_t size) { ((std::vector<uint8_t>*)u)->resize((size_t)size); return 0; },
                         [](void* u, uint64_t off, const void* buf, size_t len) {
                             memcpy(((std::vector<uint8_t>*)u)->data() + off, buf, len);
                             return 0;
                         },
                         nullptr};
        CHECK(ipbp_pbp_to_psx(job, path.c_str(), 0, &w) == IPBP_OK && got == disc2);
        ipbp_job_destroy(job);
        detail::removeFile(path);
        detail::removeFile(basePath);
    }

    // Bad input: not raw sectors, audio first.
    {
        std::vector<uint8_t> odd(2048 * 10);
        MemIn in(odd);
        PsxDisc d;
        d.image = &in;
        MemOut pbp;
        CHECK(!psxToPbp({d}, pbp).ok());
        MemIn in2(disc);
        d.image = &in2;
        d.tracks = {PsxTrack{true, 0, 0}};
        CHECK(!psxToPbp({d}, pbp).ok());
    }
}

int main() {
    testLzrcEncoders();
    testRoundTrips();
    testKeysAndIds();
    testSignatures();
    testDeterministic();
    testTamper();
    testBadInput();
    testCancelFromCallback();
    testCancelFromThreadAndFiles();
    testCApi();
    testCApiCallbacks();
    testDumpCheck();
    testPsx();
    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
