// SPDX-License-Identifier: see LICENSE
//
// PSOne EBOOT.PBP (POPS): PSISOIMG0000 for one disc, PSTITLEIMG000000 for up to five.
//
// The layout is popstation's (as kept by PSXPackager), which PSP firmware, Adrenaline and the PS1
// emulators read:
//
//   DATA.PSAR, one disc ("PSISOIMG0000" at the PSAR, or at each disc's offset)
//     0x0000  "PSISOIMG0000", u32 end of the disc data (one disc: where STARTDAT starts)
//     0x0400  "_SLUS_00594" disc ID
//     0x0800  TOC: 10-byte entries A0 (first track), A1 (last), A2 (lead-out), then each track:
//             type (0x41 data, 0x01 audio), 0, point, MSF of its first frame in the image,
//             0, MSF of INDEX 01 (+2 s), all BCD
//     0x0BFC  u32 0x100000: where the blocks start
//     0x1220  u32 (one disc) end + 0x2d31
//     0x1228  u16 0x07ff, 0x122C title, 0x12AC 0x03
//     0x4000  block index: 32-byte entries, u32 offset (from 0x100000), u32 length
//     0x100000 blocks: 16 raw 2352-byte frames each, raw deflate; a full-size block is stored
//   DATA.PSAR, several discs: "PSTITLEIMG000000", u32 end, 4 constants; 0x200: five disc
//     offsets; disc ID, title...; discs at 0x8000-aligned offsets.
//   STARTDAT (boot screen and the rest a real PSP wants) after the last disc, taken from a base PBP.

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

#include "ipbp/ipbp.hpp"
#include "psp/iso9660.h"
#include "psp/sfo.h"
#include "util.h"

#ifdef IPBP_WITH_PSX
#include <zlib.h>
#endif

namespace ipbp {

using detail::rd16;
using detail::rd32;
using detail::wr16;
using detail::wr32;

namespace {

constexpr uint32_t kFrame = 2352;
constexpr uint32_t kBlockFrames = 16;
constexpr uint32_t kBlock = kFrame * kBlockFrames;  // 0x9300
constexpr uint32_t kIsoOffset = 0x100000;
constexpr uint32_t kIndexOffset = 0x4000;
constexpr uint32_t kIndexEntry = 32;
constexpr uint32_t kMaxBlocks = (kIsoOffset - kIndexOffset) / kIndexEntry;  // 0x7E00
constexpr unsigned kMaxDiscs = 5;

// Popstation's constants for the multi-disc header.
const uint8_t kTitleImgData3[128] = {
    0x8c, 0x0e, 0xae, 0x9d, 0x39, 0x8c, 0xfe, 0x24, 0x65, 0x21, 0xad, 0x2d, 0x65, 0xa9, 0x61, 0xdf,
    0xd4, 0x4a, 0x13, 0x03, 0xb4, 0xcd, 0x32, 0x5f, 0xbd, 0xb0, 0xf4, 0xf9, 0x8a, 0x70, 0xbe, 0x1e,
    0x39, 0x2c, 0x7d, 0xc0, 0xc1, 0xc6, 0x6b, 0x81, 0xaa, 0x3c, 0x06, 0x53, 0x94, 0x1b, 0xce, 0xe5,
    0x44, 0x16, 0xcf, 0xdb, 0xb1, 0xe3, 0x89, 0x7b, 0xa2, 0xd2, 0xe7, 0xd6, 0xc1, 0x26, 0x6b, 0x58,
    0x8d, 0x2c, 0xe6, 0xc3, 0x15, 0x97, 0xd0, 0x29, 0xc9, 0x16, 0x81, 0xb6, 0xcc, 0x42, 0xee, 0x0c,
    0x28, 0x10, 0xea, 0xf2, 0x6b, 0x6f, 0x90, 0x30, 0x05, 0xbe, 0x4a, 0x2f, 0x4a, 0xbc, 0xdc, 0xe5,
    0x87, 0xce, 0x19, 0xb9, 0x80, 0xde, 0xb8, 0x32, 0xdd, 0xad, 0x89, 0x67, 0xe0, 0x92, 0x78, 0x89,
    0xe8, 0xdc, 0x45, 0x1d, 0x0c, 0xbe, 0x8b, 0x99, 0x4d, 0x50, 0xb3, 0xd6, 0x58, 0x96, 0x61, 0x75,
};

unsigned threadsFor(unsigned t) {
    if (t) return t;
    unsigned h = std::thread::hardware_concurrency();
    return h ? (h > 16 ? 16 : h) : 2;
}

void logf(const Callbacks& cb, const char* fmt, ...) {
    if (!cb.log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cb.log(buf);
}

uint8_t bcd(uint32_t v) { return (uint8_t)(((v / 10) % 10) << 4 | (v % 10)); }
bool validBcd(uint8_t b) { return (b & 0x0f) <= 9 && (b >> 4) <= 9; }
uint32_t fromBcd(uint8_t b) { return (b >> 4) * 10u + (b & 0x0f); }
void putMsf(uint8_t* p, uint32_t frames) {
    p[0] = bcd(frames / (75 * 60));
    p[1] = bcd((frames / 75) % 60);
    p[2] = bcd(frames % 75);
}
bool getMsf(const uint8_t* p, uint32_t* frames) {
    if (!validBcd(p[0]) || !validBcd(p[1]) || !validBcd(p[2])) return false;
    *frames = (fromBcd(p[0]) * 60 + fromBcd(p[1])) * 75 + fromBcd(p[2]);
    return true;
}

// ---- disc IDs ----------------------------------------------------------------------------------

// The 2048-byte user data of a raw image, for the ISO9660 reader.
class UserData : public InputStream {
public:
    explicit UserData(InputStream& raw) : raw_(raw) {}
    uint64_t size() const override { return raw_.size() / kFrame * 2048; }
    bool readAt(uint64_t offset, void* buf, size_t len) override {
        uint8_t sector[kFrame];
        uint8_t* out = static_cast<uint8_t*>(buf);
        while (len) {
            uint64_t lba = offset / 2048;
            uint32_t in = (uint32_t)(offset % 2048);
            if (!raw_.readAt(lba * kFrame, sector, kFrame)) return false;
            uint32_t data = sector[15] == 1 ? 16 : 24;  // MODE1 / MODE2 form 1
            size_t n = std::min<size_t>(len, 2048 - in);
            memcpy(out, sector + data + in, n);
            out += n;
            offset += n;
            len -= n;
        }
        return true;
    }

private:
    InputStream& raw_;
};

std::string cleanId(const std::string& in) {
    std::string id;
    for (char c : in)
        if (isalnum((unsigned char)c)) id += (char)toupper((unsigned char)c);
    return id.substr(0, 9);
}

}  // namespace

std::string psxDiscId(InputStream& image) {
    if (image.size() < 17ull * kFrame) return std::string();
    UserData data(image);
    psp::IsoReader iso(data);
    std::vector<uint8_t> cnf;
    if (!iso.open(nullptr) || !iso.readFile("/SYSTEM.CNF", cnf)) return std::string();
    std::string text(cnf.begin(), cnf.end());
    std::string upper = text;
    for (char& c : upper) c = (char)toupper((unsigned char)c);
    size_t b = upper.find("BOOT");
    while (b != std::string::npos && (upper.compare(b, 5, "BOOT2") == 0 && upper.find("BOOT", b + 1) != std::string::npos))
        b = upper.find("BOOT", b + 1);  // BOOT2 is the PS2's
    if (b == std::string::npos) return std::string();
    size_t eq = upper.find('=', b);
    if (eq == std::string::npos) return std::string();
    size_t end = upper.find_first_of("\r\n", eq);
    std::string value = upper.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1);
    size_t colon = value.find(':');
    if (colon != std::string::npos) value = value.substr(colon + 1);
    size_t slash = value.find_last_of("\\/");
    if (slash != std::string::npos) value = value.substr(slash + 1);
    size_t semi = value.find(';');
    if (semi != std::string::npos) value = value.substr(0, semi);
    std::string id = cleanId(value);
    return id.size() >= 4 ? id : std::string();
}

#ifdef IPBP_WITH_PSX

namespace {

// ---- PARAM.SFO --------------------------------------------------------------------------------

std::vector<uint8_t> buildSfo(const std::string& discId, const std::string& title) {
    struct E {
        const char* key;
        bool isInt;
        uint32_t i;
        std::string s;
        uint32_t max;
    };
    std::string t = title.substr(0, 127);
    std::vector<E> e = {
        {"BOOTABLE", true, 1, "", 4},
        {"CATEGORY", false, 0, "ME", 4},
        {"DISC_ID", false, 0, discId, 16},
        {"DISC_VERSION", false, 0, "1.00", 8},
        {"LICENSE", false, 0, "Library programs Copyright(C) Sony ComputerEntertainment Inc.", 512},
        {"PARENTAL_LEVEL", true, 1, "", 4},
        {"PSP_SYSTEM_VER", false, 0, "3.01", 8},
        {"REGION", true, 0x8000, "", 4},
        {"TITLE", false, 0, t, 128},
    };
    uint32_t keys = 0, data = 0;
    for (auto& x : e) {
        keys += (uint32_t)strlen(x.key) + 1;
        data += x.max;
    }
    uint32_t keyOff = 20 + 16 * (uint32_t)e.size();
    uint32_t dataOff = keyOff + ((keys + 3) & ~3u);
    std::vector<uint8_t> out(dataOff + data, 0);
    memcpy(out.data(), "\0PSF", 4);
    wr32(&out[4], 0x101);
    wr32(&out[8], keyOff);
    wr32(&out[12], dataOff);
    wr32(&out[16], (uint32_t)e.size());
    uint32_t k = 0, d = 0;
    for (size_t i = 0; i < e.size(); i++) {
        uint8_t* ent = &out[20 + 16 * i];
        wr16(ent, (uint16_t)k);
        wr16(ent + 2, e[i].isInt ? 0x0404 : 0x0204);
        uint32_t len = e[i].isInt ? 4 : (uint32_t)e[i].s.size() + 1;
        wr32(ent + 4, len);
        wr32(ent + 8, e[i].max);
        wr32(ent + 12, d);
        memcpy(&out[keyOff + k], e[i].key, strlen(e[i].key));
        if (e[i].isInt)
            wr32(&out[dataOff + d], e[i].i);
        else
            memcpy(&out[dataOff + d], e[i].s.data(), e[i].s.size());
        k += (uint32_t)strlen(e[i].key) + 1;
        d += e[i].max;
    }
    return out;
}

// ---- writing --------------------------------------------------------------------------------

struct Writer {
    OutputStream& out;
    uint64_t pos = 0;
    bool ok = true;
    bool put(const void* p, size_t n) {
        if (n && !out.writeAt(pos, p, n)) ok = false;
        pos += n;
        return ok;
    }
    bool zeros(uint64_t n) {
        static const uint8_t z[4096] = {};
        while (n && ok) {
            size_t k = (size_t)std::min<uint64_t>(n, sizeof(z));
            put(z, k);
            n -= k;
        }
        return ok;
    }
    bool alignTo(uint64_t a) { return zeros((a - pos % a) % a); }
    bool at(uint64_t where, const void* p, size_t n) { return out.writeAt(where, p, n) || (ok = false); }
};

std::vector<uint8_t> buildToc(const std::vector<PsxTrack>& tracks, uint32_t frames) {
    std::vector<uint8_t> toc(10 * (tracks.size() + 3), 0);
    auto type = [](const PsxTrack& t) { return (uint8_t)(t.audio ? 0x01 : 0x41); };
    uint8_t* a0 = &toc[0];
    a0[0] = type(tracks.front());
    a0[2] = 0xA0;
    a0[7] = 0x01;
    a0[8] = 0x20;  // CD-ROM XA
    uint8_t* a1 = &toc[10];
    a1[0] = type(tracks.back());
    a1[2] = 0xA1;
    a1[7] = bcd((uint32_t)tracks.size());
    uint8_t* a2 = &toc[20];
    a2[0] = type(tracks.front());
    a2[2] = 0xA2;
    putMsf(a2 + 7, frames);
    for (size_t i = 0; i < tracks.size(); i++) {
        uint8_t* t = &toc[30 + 10 * i];
        t[0] = type(tracks[i]);
        t[2] = bcd((uint32_t)i + 1);
        // Track 1's pregap is the 2 s lead-in, never in the image; the others' are where the image
        // has them (readers take the frames between these two fields as the pregap).
        putMsf(t + 3, i == 0 ? tracks[i].index1 : tracks[i].start + 150);
        putMsf(t + 7, tracks[i].index1 + 150);
    }
    return toc;
}

struct DiscPlan {
    const PsxDisc* disc;
    std::string id;
    std::vector<PsxTrack> tracks;
    uint32_t frames = 0;
    uint32_t blocks = 0;
};

// One PSISOIMG0000 at w.pos. Returns false on error (with *err set).
Result writeDisc(Writer& w, const DiscPlan& d, const std::string& title, bool multi, uint64_t psar,
                 const PsxToPbpOptions& opt, const Callbacks& cb, detail::WorkerPool& pool, uint64_t& done,
                 uint64_t total, uint64_t* p1Pos, uint64_t* p2Pos) {
    const uint64_t start = w.pos;
    std::vector<uint8_t> head(kIsoOffset, 0);
    memcpy(&head[0], "PSISOIMG0000", 12);
    wr32(&head[0x0C], d.blocks * kBlock + kIsoOffset);  // replaced by the end offset for one disc
    head[0x400] = '_';
    memcpy(&head[0x401], d.id.data(), std::min<size_t>(4, d.id.size()));
    head[0x405] = '_';
    if (d.id.size() > 4) memcpy(&head[0x406], d.id.data() + 4, std::min<size_t>(5, d.id.size() - 4));
    std::vector<uint8_t> toc = buildToc(d.tracks, d.frames);
    memcpy(&head[0x800], toc.data(), toc.size());
    wr32(&head[0xBFC], kIsoOffset);
    wr16(&head[0x1228], 0x07ff);
    std::string t = title.substr(0, 127);
    memcpy(&head[0x122C], t.data(), t.size());
    head[0x12AC] = 0x03;
    if (!multi && p1Pos) *p1Pos = start + 0x0C;
    if (!multi && p2Pos) *p2Pos = start + 0x1220;
    if (!w.put(head.data(), head.size())) return Result::fail(Status::IoError, "write failed");

    // Blocks, compressed in parallel batches, written in order.
    const unsigned threads = pool.size();
    const size_t batch = std::max<size_t>(4, threads * 4);
    struct Slot {
        std::vector<uint8_t> raw, packed;
        uint32_t size = 0;
        bool stored = false;
        bool readOk = true;
    };
    std::vector<Slot> slots(batch);
    for (auto& s : slots) {
        s.raw.resize(kBlock);
        s.packed.resize(kBlock + 1024);
    }
    const uint64_t imageBytes = (uint64_t)d.frames * kFrame;
    std::vector<uint32_t> offs(d.blocks), lens(d.blocks);
    uint32_t dataOff = 0;
    std::mutex readMutex;
    for (uint32_t b0 = 0; b0 < d.blocks; b0 += (uint32_t)batch) {
        if (cb.cancel.cancelled()) return Result::fail(Status::Cancelled, "cancelled");
        const size_t n = std::min<size_t>(batch, d.blocks - b0);
        pool.run(n, [&](size_t i) {
            Slot& s = slots[i];
            const uint64_t off = (uint64_t)(b0 + i) * kBlock;
            const size_t have = (size_t)std::min<uint64_t>(kBlock, imageBytes - off);
            {
                std::lock_guard<std::mutex> l(readMutex);
                s.readOk = d.disc->image->readAt(off, s.raw.data(), have);
            }
            if (have < kBlock) memset(s.raw.data() + have, 0, kBlock - have);
            s.stored = true;
            s.size = kBlock;
            if (opt.level <= 0 || !s.readOk) return;
            z_stream z;
            memset(&z, 0, sizeof(z));
            if (deflateInit2(&z, std::min(opt.level, 9), Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return;
            z.next_in = s.raw.data();
            z.avail_in = kBlock;
            z.next_out = s.packed.data();
            z.avail_out = (uInt)s.packed.size();
            int rc = deflate(&z, Z_FINISH);
            if (rc == Z_STREAM_END && z.total_out < kBlock) {
                s.size = (uint32_t)z.total_out;
                s.stored = false;
            }
            deflateEnd(&z);
        });
        for (size_t i = 0; i < n; i++) {
            Slot& s = slots[i];
            if (!s.readOk) return Result::fail(Status::IoError, "cannot read the disc image");
            offs[b0 + i] = dataOff;
            lens[b0 + i] = s.size;
            if (!w.put(s.stored ? s.raw.data() : s.packed.data(), s.size))
                return Result::fail(Status::IoError, "write failed");
            dataOff += s.size;
        }
        done += (uint64_t)n * kBlock;
        if (cb.progress) cb.progress(std::min(done, total), total);
    }
    // The index.
    std::vector<uint8_t> index((size_t)d.blocks * kIndexEntry, 0);
    for (uint32_t i = 0; i < d.blocks; i++) {
        wr32(&index[(size_t)i * kIndexEntry], offs[i]);
        wr32(&index[(size_t)i * kIndexEntry + 4], lens[i]);
    }
    if (!w.at(start + kIndexOffset, index.data(), index.size())) return Result::fail(Status::IoError, "write failed");
    (void)psar;
    return Result::success();
}

struct BasePbp {
    std::vector<uint8_t> icon0, dataPsp, startdatHead, startdatPng;
    std::unique_ptr<InputStream> file;
    uint64_t restOffset = 0;  // the rest of STARTDAT, copied to the end
};

Result loadBase(const std::string& path, BasePbp& b) {
    std::string err;
    b.file = openInputFile(path, &err);
    if (!b.file) return Result::fail(Status::IoError, err);
    InputStream& in = *b.file;
    uint8_t h[0x28];
    if (in.size() < 0x28 || !in.readAt(0, h, sizeof(h)) || memcmp(h, "\0PBP", 4) != 0)
        return Result::fail(Status::InvalidInput, path + " is not a PBP");
    uint32_t off[8];
    for (int i = 0; i < 8; i++) off[i] = rd32(h + 8 + 4 * i);
    auto readRange = [&](uint32_t a, uint32_t z, std::vector<uint8_t>& out) {
        if (z <= a || z > in.size()) return false;
        out.resize(z - a);
        return in.readAt(a, out.data(), out.size());
    };
    readRange(off[1], off[2], b.icon0);
    // DATA.PSP: its own size is at 0x2C of its header.
    uint8_t ph[0x30];
    if (off[7] > off[6] + 0x30 && in.readAt(off[6], ph, sizeof(ph))) {
        uint32_t sz = rd32(ph + 0x2C);
        if (sz == 0 || sz > off[7] - off[6]) sz = off[7] - off[6];
        readRange(off[6], off[6] + sz, b.dataPsp);
    }
    if (b.dataPsp.empty()) return Result::fail(Status::InvalidInput, path + " has no DATA.PSP");
    // STARTDAT: where the PSAR header says the discs end, else searched for.
    const uint64_t psar = off[7];
    uint8_t m[20];
    uint64_t sd = 0;
    if (psar + 20 <= in.size() && in.readAt(psar, m, sizeof(m))) {
        uint32_t end = memcmp(m, "PSTITLEIMG000000", 16) == 0 ? rd32(m + 16) : memcmp(m, "PSISOIMG0000", 12) == 0 ? rd32(m + 12) : 0;
        uint8_t tag[8];
        for (uint64_t cand : {psar + end, (uint64_t)end, (uint64_t)end + 0x50000})
            if (end && cand + 8 <= in.size() && in.readAt(cand, tag, 8) && memcmp(tag, "STARTDAT", 8) == 0) {
                sd = cand;
                break;
            }
    }
    if (!sd) {
        std::vector<uint8_t> buf(1 << 20);
        for (uint64_t p = psar; p < in.size() && !sd; p += buf.size() - 16) {
            size_t n = (size_t)std::min<uint64_t>(buf.size(), in.size() - p);
            if (!in.readAt(p, buf.data(), n)) break;
            for (size_t i = 0; i + 8 <= n; i += 16)
                if (memcmp(&buf[i], "STARTDAT", 8) == 0) {
                    sd = p + i;
                    break;
                }
            if (n < buf.size()) break;
        }
    }
    if (!sd) return Result::fail(Status::InvalidInput, path + " has no STARTDAT: not a PSOne EBOOT");
    uint8_t sh[24];
    if (!in.readAt(sd, sh, sizeof(sh))) return Result::fail(Status::IoError, "cannot read " + path);
    uint32_t hs = rd32(sh + 16), ps = rd32(sh + 20);
    if (hs < 24 || hs > 0x1000 || sd + hs + ps > in.size()) return Result::fail(Status::InvalidInput, path + ": bad STARTDAT");
    b.startdatHead.resize(hs);
    in.readAt(sd, b.startdatHead.data(), hs);
    b.startdatPng.resize(ps);
    if (ps) in.readAt(sd + hs, b.startdatPng.data(), ps);
    b.restOffset = sd + hs + ps;
    return Result::success();
}

}  // namespace

Result psxToPbp(const std::vector<PsxDisc>& discs, OutputStream& out, const PsxToPbpOptions& opt, const Callbacks& cb,
                uint64_t* outSize) {
    if (discs.empty() || discs.size() > kMaxDiscs) return Result::fail(Status::InvalidInput, "a PSOne EBOOT holds 1 to 5 discs");
    std::vector<DiscPlan> plan(discs.size());
    uint64_t total = 0;
    for (size_t i = 0; i < discs.size(); i++) {
        DiscPlan& p = plan[i];
        p.disc = &discs[i];
        if (!p.disc->image) return Result::fail(Status::InvalidInput, "missing disc image");
        const uint64_t size = p.disc->image->size();
        if (size == 0 || size % kFrame)
            return Result::fail(Status::InvalidInput, "a PSOne disc image must be raw 2352-byte sectors (a .bin)");
        p.frames = (uint32_t)(size / kFrame);
        p.blocks = (p.frames + kBlockFrames - 1) / kBlockFrames;
        if (p.blocks > kMaxBlocks) return Result::fail(Status::Unsupported, "the disc is too big for a PSOne EBOOT");
        p.tracks = p.disc->tracks;
        if (p.tracks.empty()) p.tracks.push_back(PsxTrack{});
        if (p.tracks.size() > 99) return Result::fail(Status::InvalidInput, "more than 99 tracks");
        for (size_t t = 0; t < p.tracks.size(); t++) {
            const PsxTrack& tr = p.tracks[t];
            if (tr.start > tr.index1 || tr.index1 >= p.frames || (t && tr.start < p.tracks[t - 1].index1))
                return Result::fail(Status::InvalidInput, "the track list doesn't fit the image");
        }
        if (p.tracks.front().audio) return Result::fail(Status::InvalidInput, "track 1 of a PSOne disc must be data");
        p.id = cleanId(p.disc->discId);
        if (p.id.empty()) p.id = psxDiscId(*p.disc->image);
        if (p.id.empty()) {
            p.id = "SLUS00000";
            logf(cb, "disc %zu: no SYSTEM.CNF, using the ID %s", i + 1, p.id.c_str());
        }
        while (p.id.size() < 9) p.id += '0';
        total += (uint64_t)p.blocks * kBlock;
    }
    const std::string title = opt.title.empty() ? plan[0].id : opt.title;

    BasePbp base;
    if (!opt.basePbpPath.empty()) {
        Result r = loadBase(opt.basePbpPath, base);
        if (!r) return r;
    } else if (!opt.bootPng.empty()) {
        logf(cb, "a boot screen needs a base PBP (its STARTDAT): left out");
    }

    // Header and the files before DATA.PSAR.
    std::vector<uint8_t> sfo = buildSfo(plan[0].id, title);
    const std::vector<uint8_t>& icon0 = !opt.icon0.empty() ? opt.icon0 : base.icon0;
    const std::vector<uint8_t>* parts[7] = {&sfo, &icon0, nullptr, &opt.pic0, &opt.pic1, &opt.snd0, &base.dataPsp};
    uint8_t header[0x28] = {};
    memcpy(header, "\0PBP", 4);
    wr32(header + 4, 0x10000);
    uint32_t pos = 0x28;
    for (int i = 0; i < 7; i++) {
        wr32(header + 8 + 4 * i, pos);
        if (parts[i]) pos += (uint32_t)parts[i]->size();
    }
    const uint64_t psar = (pos + 0xFFFFu) & ~0xFFFFull;
    wr32(header + 0x24, (uint32_t)psar);

    Writer w{out};
    w.put(header, sizeof(header));
    for (auto* p : parts)
        if (p) w.put(p->data(), p->size());
    w.zeros(psar - w.pos);
    if (!w.ok) return Result::fail(Status::IoError, "write failed");

    detail::WorkerPool pool(threadsFor(opt.threads));
    uint64_t done = 0, p1 = 0, p2 = 0;
    const bool multi = discs.size() > 1;
    std::vector<uint32_t> discOffsets;
    uint64_t multiP1 = 0, multiP2 = 0, multiPositions = 0;
    if (multi) {
        std::vector<uint8_t> h(0x3DC, 0);
        memcpy(&h[0], "PSTITLEIMG000000", 16);
        multiP1 = psar + 0x10;
        wr32(&h[0x18], 0x2CC9C5BC);
        wr32(&h[0x1C], 0x33B5A90F);
        wr32(&h[0x20], 0x06F6B4B3);
        wr32(&h[0x24], 0xB25945BA);
        multiPositions = psar + 0x200;
        std::mt19937 rng(0x1234567u ^ plan[0].frames);
        for (int i = 0; i < 12; i++) h[0x214 + i] = (uint8_t)rng();
        h[0x240] = '_';
        memcpy(&h[0x241], plan[0].id.data(), 4);
        h[0x245] = '_';
        memcpy(&h[0x246], plan[0].id.data() + 4, 5);
        multiP2 = psar + 0x260;
        memcpy(&h[0x268], kTitleImgData3, sizeof(kTitleImgData3));
        std::string t = title.substr(0, 127);
        memcpy(&h[0x2E8], t.data(), t.size());
        wr32(&h[0x368], 7);
        w.put(h.data(), h.size());
    }
    for (size_t i = 0; i < plan.size(); i++) {
        if (multi) {
            w.alignTo(0x8000);
            discOffsets.push_back((uint32_t)(w.pos - psar));
        }
        logf(cb, "disc %zu: %s, %zu track%s, %u frames", i + 1, plan[i].id.c_str(), plan[i].tracks.size(),
             plan[i].tracks.size() == 1 ? "" : "s", plan[i].frames);
        Result r = writeDisc(w, plan[i], title, multi, psar, opt, cb, pool, done, total, &p1, &p2);
        if (!r) return r;
    }
    if (!w.ok) return Result::fail(Status::IoError, "write failed");
    w.alignTo(0x10);
    const uint32_t end = (uint32_t)(w.pos - psar);
    uint8_t v[4];
    wr32(v, end);
    if (multi) {
        w.at(multiP1, v, 4);
        wr32(v, end + 0x2d31);
        w.at(multiP2, v, 4);
        uint8_t positions[20] = {};
        for (size_t i = 0; i < discOffsets.size() && i < kMaxDiscs; i++) wr32(positions + 4 * i, discOffsets[i]);
        w.at(multiPositions, positions, sizeof(positions));
    } else {
        w.at(p1, v, 4);
        wr32(v, end + 0x2d31);
        w.at(p2, v, 4);
    }

    // STARTDAT, from the base PBP.
    if (base.file) {
        std::vector<uint8_t> head = base.startdatHead;
        const std::vector<uint8_t>& png = opt.bootPng.empty() ? base.startdatPng : opt.bootPng;
        wr32(&head[20], (uint32_t)png.size());
        w.put(head.data(), head.size());
        w.put(png.data(), png.size());
        std::vector<uint8_t> buf(1 << 20);
        for (uint64_t p = base.restOffset; p < base.file->size();) {
            size_t n = (size_t)std::min<uint64_t>(buf.size(), base.file->size() - p);
            if (!base.file->readAt(p, buf.data(), n)) return Result::fail(Status::IoError, "cannot read the base PBP");
            w.put(buf.data(), n);
            p += n;
        }
    }
    if (!w.ok || !out.finish(w.pos)) return Result::fail(Status::IoError, "write failed");
    if (outSize) *outSize = w.pos;
    if (cb.progress) cb.progress(total, total);
    return Result::success();
}

// ---- reading --------------------------------------------------------------------------------

namespace {

struct PbpDisc {
    uint64_t start = 0;  // absolute
    uint32_t isoOffset = kIsoOffset;
    std::vector<uint32_t> offs, lens;
};

Result psarOf(InputStream& in, uint64_t* psar, uint8_t magic[16]) {
    uint8_t h[0x28];
    if (in.size() < 0x28 || !in.readAt(0, h, sizeof(h)) || memcmp(h, "\0PBP", 4) != 0)
        return Result::fail(Status::InvalidInput, "not a PBP file");
    *psar = rd32(h + 0x24);
    if (*psar + 16 > in.size() || !in.readAt(*psar, magic, 16)) return Result::fail(Status::InvalidInput, "no DATA.PSAR");
    return Result::success();
}

Result discOffsets(InputStream& in, std::vector<uint64_t>& out) {
    uint64_t psar;
    uint8_t m[16];
    Result r = psarOf(in, &psar, m);
    if (!r) return r;
    if (memcmp(m, "PSISOIMG0000", 12) == 0) {
        out.push_back(psar);
        return Result::success();
    }
    if (memcmp(m, "PSTITLEIMG000000", 16) != 0) {
        if (memcmp(m, "\0PGD", 4) == 0) return Result::fail(Status::Unsupported, "an encrypted (PSN) PSOne EBOOT");
        return Result::fail(Status::InvalidInput, "not a PSOne EBOOT");
    }
    uint8_t t[20];
    if (!in.readAt(psar + 0x200, t, sizeof(t))) return Result::fail(Status::IoError, "read failed");
    if (rd32(t) == 0x44475000) return Result::fail(Status::Unsupported, "an encrypted (PSN) PSOne EBOOT");
    for (int i = 0; i < 5 && rd32(t + 4 * i); i++) out.push_back(psar + rd32(t + 4 * i));
    if (out.empty()) return Result::fail(Status::InvalidInput, "no discs in the PSOne EBOOT");
    return Result::success();
}

Result readDisc(InputStream& in, uint64_t start, PbpDisc& d, PsxDiscInfo& info) {
    std::vector<uint8_t> h(kIndexOffset);
    if (start + kIndexOffset > in.size() || !in.readAt(start, h.data(), h.size()))
        return Result::fail(Status::InvalidInput, "truncated disc header");
    if (memcmp(h.data(), "PSISOIMG0000", 12) != 0) return Result::fail(Status::InvalidInput, "no PSISOIMG0000 header");
    d.start = start;
    std::string id;
    for (int i : {1, 2, 3, 4, 6, 7, 8, 9, 10})
        if (isalnum(h[0x400 + i])) id += (char)h[0x400 + i];
    info.discId = id;
    d.isoOffset = rd32(&h[0xBFC]) ? rd32(&h[0xBFC]) : kIsoOffset;
    if (d.isoOffset <= kIndexOffset || d.isoOffset > (64u << 20)) d.isoOffset = kIsoOffset;
    // The block index.
    const uint32_t maxEntries = (d.isoOffset - kIndexOffset) / kIndexEntry;
    std::vector<uint8_t> idx((size_t)maxEntries * kIndexEntry);
    if (start + d.isoOffset > in.size() || !in.readAt(start + kIndexOffset, idx.data(), idx.size()))
        return Result::fail(Status::InvalidInput, "truncated block index");
    for (uint32_t i = 0; i < maxEntries; i++) {
        uint32_t off = rd32(&idx[(size_t)i * kIndexEntry]);
        uint32_t len = rd32(&idx[(size_t)i * kIndexEntry + 4]);
        if (len > kBlock) len &= 0xFFFF;
        if (len == 0 || len > kBlock) break;
        d.offs.push_back(off);
        d.lens.push_back(len);
        info.storedBytes += len;
    }
    if (d.offs.empty()) return Result::fail(Status::InvalidInput, "empty block index");
    info.blocks = (uint32_t)d.offs.size();
    const uint32_t maxFrames = info.blocks * kBlockFrames;
    // The TOC.
    const uint8_t* toc = &h[0x800];
    uint32_t leadout = 0, first = 0, last = 0;
    bool tocOk = toc[2] == 0xA0 && toc[12] == 0xA1 && toc[22] == 0xA2 && validBcd(toc[7]) && validBcd(toc[17]);
    if (tocOk) {
        first = fromBcd(toc[7]);
        last = fromBcd(toc[17]);
        tocOk = first == 1 && last >= 1 && last <= 99 && getMsf(toc + 27, &leadout);
    }
    info.frames = tocOk && leadout > 0 && leadout <= maxFrames ? leadout : maxFrames;
    info.tracks.clear();
    if (tocOk) {
        for (uint32_t t = 0; t < last; t++) {
            const uint8_t* e = toc + 30 + 10 * t;
            uint32_t a, b;
            if (fromBcd(e[2]) != t + 1 || !getMsf(e + 3, &a) || !getMsf(e + 7, &b) || b < 150) {
                info.tracks.clear();
                break;
            }
            PsxTrack tr;
            tr.audio = (e[0] & 0x40) == 0;  // control: data bit
            tr.index1 = b - 150;
            tr.start = t == 0 ? 0 : (a >= 150 ? a - 150 : tr.index1);
            if (tr.start > tr.index1) tr.start = tr.index1;
            if (tr.index1 >= info.frames || (t && tr.start < info.tracks.back().index1)) {
                info.tracks.clear();
                break;
            }
            info.tracks.push_back(tr);
        }
    }
    if (info.tracks.empty()) info.tracks.push_back(PsxTrack{});
    return Result::success();
}

bool decodeBlock(InputStream& in, const PbpDisc& d, uint32_t b, uint8_t* out, std::vector<uint8_t>& tmp) {
    const uint64_t at = d.start + d.isoOffset + d.offs[b];
    const uint32_t len = d.lens[b];
    if (at + len > in.size()) return false;
    if (len == kBlock) return in.readAt(at, out, kBlock);
    tmp.resize(len);
    if (!in.readAt(at, tmp.data(), len)) return false;
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK) return false;
    z.next_in = tmp.data();
    z.avail_in = len;
    z.next_out = out;
    z.avail_out = kBlock;
    int rc = inflate(&z, Z_FINISH);
    bool ok = (rc == Z_STREAM_END || rc == Z_BUF_ERROR || rc == Z_OK) && z.total_out > 0;
    if (z.total_out < kBlock) memset(out + z.total_out, 0, kBlock - z.total_out);
    inflateEnd(&z);
    return ok;
}

Result openPsx(InputStream& in, PsxInfo& info, std::vector<PbpDisc>& discs) {
    std::vector<uint64_t> starts;
    Result r = discOffsets(in, starts);
    if (!r) return r;
    uint8_t h[0x28];
    in.readAt(0, h, sizeof(h));
    uint32_t sfoAt = rd32(h + 8), sfoEnd = rd32(h + 12), pspAt = rd32(h + 0x20), psarAt = rd32(h + 0x24);
    if (sfoEnd > sfoAt && sfoEnd - sfoAt < (1u << 20) && sfoEnd <= in.size()) {
        std::vector<uint8_t> sfo(sfoEnd - sfoAt);
        if (in.readAt(sfoAt, sfo.data(), sfo.size())) {
            psp::Sfo s(sfo);
            s.getString("TITLE", info.title);
            s.getString("DISC_ID", info.discId);
        }
    }
    // DATA.PSP: something there (the space before DATA.PSAR is padding otherwise).
    uint8_t psp[0x30] = {};
    info.hasDataPsp = psarAt >= pspAt + 0x30 && in.readAt(pspAt, psp, sizeof(psp)) &&
                      std::any_of(psp, psp + sizeof(psp), [](uint8_t c) { return c != 0; });
    for (uint64_t s : starts) {
        PbpDisc d;
        PsxDiscInfo di;
        r = readDisc(in, s, d, di);
        if (!r) return r;
        discs.push_back(std::move(d));
        info.discs.push_back(std::move(di));
    }
    // Data tracks' modes, from their first sector.
    std::vector<uint8_t> block(kBlock), tmp;
    for (size_t i = 0; i < discs.size(); i++) {
        PsxDiscInfo& di = info.discs[i];
        di.dataModes.assign(di.tracks.size(), 0);
        for (size_t t = 0; t < di.tracks.size(); t++) {
            if (di.tracks[t].audio) continue;
            uint32_t f = di.tracks[t].index1;
            di.dataModes[t] = 2;
            if (decodeBlock(in, discs[i], f / kBlockFrames, block.data(), tmp)) {
                uint8_t mode = block[(f % kBlockFrames) * kFrame + 15];
                if (mode == 1 || mode == 2) di.dataModes[t] = mode;
            }
        }
    }
    return Result::success();
}

}  // namespace

Result readPsxInfo(InputStream& in, PsxInfo& info) {
    info = PsxInfo();
    std::vector<PbpDisc> discs;
    return openPsx(in, info, discs);
}

Result pbpToPsx(InputStream& in, unsigned disc, OutputStream& image, const Callbacks& cb, uint64_t* outSize) {
    PsxInfo info;
    std::vector<PbpDisc> discs;
    Result r = openPsx(in, info, discs);
    if (!r) return r;
    if (disc >= discs.size()) return Result::fail(Status::InvalidInput, "the EBOOT has no disc " + std::to_string(disc + 1));
    const PbpDisc& d = discs[disc];
    const uint64_t bytes = (uint64_t)info.discs[disc].frames * kFrame;
    const uint32_t blocks = (uint32_t)((bytes + kBlock - 1) / kBlock);
    detail::WorkerPool pool(threadsFor(0));
    const size_t batch = std::max<size_t>(4, pool.size() * 4);
    std::vector<std::vector<uint8_t>> out(batch, std::vector<uint8_t>(kBlock)), tmp(batch);
    std::vector<char> ok(batch);
    std::mutex readMutex;
    for (uint32_t b0 = 0; b0 < blocks; b0 += (uint32_t)batch) {
        if (cb.cancel.cancelled()) return Result::fail(Status::Cancelled, "cancelled");
        const size_t n = std::min<size_t>(batch, blocks - b0);
        pool.run(n, [&](size_t i) {
            const uint32_t b = b0 + (uint32_t)i;
            if (b >= d.offs.size()) {
                memset(out[i].data(), 0, kBlock);
                ok[i] = 1;
                return;
            }
            // The reads share the stream; inflating is what runs in parallel.
            const uint64_t at = d.start + d.isoOffset + d.offs[b];
            const uint32_t len = d.lens[b];
            {
                std::lock_guard<std::mutex> l(readMutex);
                tmp[i].resize(len);
                ok[i] = at + len <= in.size() && in.readAt(at, tmp[i].data(), len);
            }
            if (!ok[i]) return;
            if (len == kBlock) {
                memcpy(out[i].data(), tmp[i].data(), kBlock);
                return;
            }
            z_stream z;
            memset(&z, 0, sizeof(z));
            ok[i] = inflateInit2(&z, -15) == Z_OK;
            if (!ok[i]) return;
            z.next_in = tmp[i].data();
            z.avail_in = len;
            z.next_out = out[i].data();
            z.avail_out = kBlock;
            int rc = inflate(&z, Z_FINISH);
            ok[i] = rc == Z_STREAM_END || (rc == Z_BUF_ERROR && z.total_out == kBlock);
            if (z.total_out < kBlock) memset(out[i].data() + z.total_out, 0, kBlock - z.total_out);
            inflateEnd(&z);
        });
        for (size_t i = 0; i < n; i++) {
            if (!ok[i]) return Result::fail(Status::InvalidInput, "block " + std::to_string(b0 + i) + " doesn't decompress");
            const uint64_t off = (uint64_t)(b0 + i) * kBlock;
            const size_t len = (size_t)std::min<uint64_t>(kBlock, bytes - off);
            if (!image.writeAt(off, out[i].data(), len)) return Result::fail(Status::IoError, "write failed");
        }
        if (cb.progress) cb.progress(std::min<uint64_t>((uint64_t)(b0 + n) * kBlock, bytes), bytes);
    }
    if (!image.finish(bytes)) return Result::fail(Status::IoError, "write failed");
    if (outSize) *outSize = bytes;
    return Result::success();
}

#else  // !IPBP_WITH_PSX

Result psxToPbp(const std::vector<PsxDisc>&, OutputStream&, const PsxToPbpOptions&, const Callbacks&, uint64_t*) {
    return Result::fail(Status::Unsupported, "this iPBP was built without zlib: no PSOne EBOOTs");
}
Result readPsxInfo(InputStream&, PsxInfo&) {
    return Result::fail(Status::Unsupported, "this iPBP was built without zlib: no PSOne EBOOTs");
}
Result pbpToPsx(InputStream&, unsigned, OutputStream&, const Callbacks&, uint64_t*) {
    return Result::fail(Status::Unsupported, "this iPBP was built without zlib: no PSOne EBOOTs");
}

#endif

PbpKind pbpKind(InputStream& in) {
    uint8_t h[0x28];
    if (in.size() < 0x28 || !in.readAt(0, h, sizeof(h)) || memcmp(h, "\0PBP", 4) != 0) return PbpKind::Unknown;
    uint64_t psar = rd32(h + 0x24);
    uint8_t m[16];
    if (psar + 16 > in.size() || !in.readAt(psar, m, sizeof(m))) return PbpKind::Unknown;
    if (memcmp(m, "NPUMDIMG", 8) == 0) return PbpKind::Psp;
    if (memcmp(m, "PSISOIMG0000", 12) == 0 || memcmp(m, "PSTITLEIMG000000", 16) == 0) return PbpKind::Psx;
    return PbpKind::Unknown;
}

Result psxToPbp(const std::vector<PsxDisc>& discs, const std::string& pbpPath, const PsxToPbpOptions& opt,
                const Callbacks& cb) {
    std::string err;
    std::string part = pbpPath + ".part";
    Result r;
    {
        auto out = openOutputFile(part, &err);
        if (!out) return Result::fail(Status::IoError, err);
        r = psxToPbp(discs, *out, opt, cb);
    }
    if (!r) {
        detail::removeFile(part);
        return r;
    }
    if (!detail::renameReplace(part, pbpPath)) {
        detail::removeFile(part);
        return Result::fail(Status::IoError, "cannot rename '" + part + "' to '" + pbpPath + "'");
    }
    return r;
}

}  // namespace ipbp
