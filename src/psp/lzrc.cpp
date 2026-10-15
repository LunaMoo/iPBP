// SPDX-License-Identifier: see LICENSE
#include "lzrc.h"

#include <cstring>
#include <memory>
#include <vector>

namespace ipbp {
namespace psp {
namespace {

struct Model {
    uint8_t literal[8][256];
    uint8_t distBits[8][39];
    uint8_t dist[18][8];
    uint8_t match[8][8];
    uint8_t len[8][31];
    Model() { memset(this, 0x80, sizeof(*this)); }
};

// ------------------------------------------------------------------ decoder

struct Decoder {
    const uint8_t* in;
    int pos, len;
    uint32_t range = 0xFFFFFFFF, code = 0;
    bool err = false;

    uint8_t next() {
        if (pos >= len) {
            err = true;
            return 0;
        }
        return in[pos++];
    }
    void normalize() {
        if (range < 0x01000000) {
            range <<= 8;
            code = (code << 8) | next();
        }
    }
    int bit(uint8_t* p) {
        normalize();
        uint32_t bound = (range >> 8) * *p;
        *p -= *p >> 3;
        if (code < bound) {
            range = bound;
            *p += 31;
            return 1;
        }
        code -= bound;
        range -= bound;
        return 0;
    }
    int number(uint8_t* p, int n) {
        int num = 1;
        if (n > 3) {
            num = (num << 1) + bit(p + 3);
            if (n > 4) {
                num = (num << 1) + bit(p + 3);
                if (n > 5) {
                    normalize();
                    for (int i = 0; i < n - 5; i++) {
                        range >>= 1;
                        num <<= 1;
                        if (code < range) num += 1;
                        else code -= range;
                    }
                }
            }
        }
        if (n > 0) {
            num = (num << 1) + bit(p);
            if (n > 1) {
                num = (num << 1) + bit(p + 1);
                if (n > 2) num = (num << 1) + bit(p + 2);
            }
        }
        return num;
    }
};

// ------------------------------------------------------------------ encoder

struct Encoder {
    uint8_t* out;
    int pos = 0, cap;
    uint32_t range = 0xFFFFFFFF, low = 0;
    uint32_t cache = 0xFFFFFFFF;  // pending top byte (+carry), 0xFFFFFFFF = none
    bool overflow = false;

    void put(uint8_t b) {
        if (pos >= cap) {
            overflow = true;
            return;
        }
        out[pos++] = b;
    }
    void normalize() {
        if (range < 0x01000000) {
            if (cache != 0xFFFFFFFF) {
                if (cache > 0xFF) {  // propagate carry into already written bytes
                    for (int p = pos - 1; p >= 0; p--)
                        if (++out[p] != 0) break;
                }
                put((uint8_t)cache);
            }
            cache = low >> 24;
            range <<= 8;
            low <<= 8;
        }
    }
    void addLow(uint32_t v) {
        uint32_t old = low;
        low += v;
        if (low < old) cache++;
    }
    void bit(uint8_t* p, int b) {
        normalize();
        uint32_t bound = (range >> 8) * *p;
        *p -= *p >> 3;
        if (b) {
            range = bound;
            *p += 31;
        } else {
            addLow(bound);
            range -= bound;
        }
    }
    void bittree(uint8_t* probs, int max, int number) {
        number += max;
        int n = 0;
        for (int t = number; t > 1; t >>= 1) n++;
        do {
            bit(probs + (number >> n), (number >> (n - 1)) & 1);
            n--;
        } while (n);
    }
    void numberBits(uint8_t* p, int n, int number) {
        if (n > 3) {
            bit(p + 3, (number >> (n - 1)) & 1);
            if (n > 4) {
                bit(p + 3, (number >> (n - 2)) & 1);
                if (n > 5) {
                    normalize();
                    for (int j = n - 3; j >= 3; j--) {
                        range >>= 1;
                        if (!((number >> j) & 1)) addLow(range);
                    }
                }
            }
        }
        int low3 = n < 3 ? n : 3;
        for (int k = 0; k < low3; k++) bit(p + k, (number >> (low3 - 1 - k)) & 1);
    }
    void flush() {
        range = 0;  // force a final normalize of the pending byte
        if (cache != 0xFFFFFFFF) {
            if (cache > 0xFF)
                for (int p = pos - 1; p >= 0; p--)
                    if (++out[p] != 0) break;
            put((uint8_t)cache);
        }
        put((uint8_t)(low >> 24));
        put((uint8_t)(low >> 16));
        put((uint8_t)(low >> 8));
        put((uint8_t)low);
    }
};

inline int log2i(int v) {
    int n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

constexpr int kLc = 5;
constexpr int kMaxMatch = 255;    // number = len-1 <= 254 (0xFF is the end marker)
constexpr int kMaxDist = 0xFEFF;  // keep inside the original 0xFF00 window

}  // namespace

int lzrcDecompress(uint8_t* out, int outCap, const uint8_t* in, int inLen) {
    if (inLen < 5) return -1;
    Decoder d{in, 0, inLen};
    uint8_t lc = d.next();
    d.code = (uint32_t)d.next() << 24;
    d.code |= (uint32_t)d.next() << 16;
    d.code |= (uint32_t)d.next() << 8;
    d.code |= d.next();
    if (lc & 0x80) {  // stored
        if (d.code > (uint32_t)outCap || d.code > (uint32_t)(inLen - 5)) return -1;
        memcpy(out, in + 5, d.code);
        return (int)d.code;
    }
    Model* m = new Model;
    std::unique_ptr<Model> guard(m);
    int state = 0, outPos = 0;
    uint8_t last = 0;
    for (;;) {
        if (d.err) return -1;
        if (!d.bit(&m->match[state][0])) {
            if (state > 0) state--;
            uint8_t* probs = m->literal[(last >> lc) & 7];
            int i = 1;
            while (i < 0x100) i = (i << 1) + d.bit(&probs[i]);
            if (outPos >= outCap) return -1;
            out[outPos++] = (uint8_t)i;
        } else {
            int n = 0;
            while (n < 7 && d.bit(&m->match[state][n + 1])) n++;
            int matchLen = 1;
            if (n > 0) {
                matchLen = d.number(&m->len[state][(n - 1) * 4 + ((outPos << (n - 1)) & 3)], n);
                if (matchLen == 0xFF) return d.err ? -1 : outPos;
            }
            int limit = matchLen > 2 ? 0x2C : 8;
            uint8_t* probs = &m->distBits[n][matchLen > 2 ? 7 : 0];
            int i = 1;
            do {
                i = (i << 1) + d.bit(&probs[i]);
            } while (i < limit);
            int db = i - limit;
            if (db >= 18) return -1;
            int dist = db > 0 ? d.number(m->dist[db], db) : 1;
            if (dist > outPos || dist <= 0) return -1;
            if (outPos + matchLen + 1 > outCap) return -1;
            for (int k = 0; k <= matchLen; k++, outPos++) out[outPos] = out[outPos - dist];
            state = ((outPos + 1) & 1) + 6;
        }
        last = out[outPos - 1];
    }
}

int lzrcCompress(uint8_t* out, int outCap, const uint8_t* in, int inLen) {
    if (outCap < 6) return -1;
    std::unique_ptr<Model> mguard(new Model);
    Model* m = mguard.get();
    Encoder e{out, 0, outCap};
    e.put((uint8_t)kLc);

    // Hash chains over 3-byte prefixes.
    constexpr int kHashBits = 15;
    std::vector<int> head(1 << kHashBits, -1), prev(inLen > 0 ? inLen : 1, -1);
    auto hash3 = [&](int p) {
        return ((in[p] << 10) ^ (in[p + 1] << 5) ^ in[p + 2]) & ((1 << kHashBits) - 1);
    };
    auto insert = [&](int p) {
        if (p + 2 < inLen) {
            int h = hash3(p);
            prev[p] = head[h];
            head[h] = p;
        }
    };
    auto findMatch = [&](int p, int& bestDist) {
        int best = 1;
        bestDist = 0;
        int maxLen = inLen - p < kMaxMatch ? inLen - p : kMaxMatch;
        if (maxLen < 2) return 1;
        // 2-byte matches only make sense at short distances (len<=3 requires dist<256)
        if (p + 2 < inLen) {
            int chain = 64;
            for (int c = head[hash3(p)]; c >= 0 && chain--; c = prev[c]) {
                int d = p - c;
                if (d > kMaxDist) break;
                if (in[c + best] != in[p + best]) continue;
                int l = 0;
                while (l < maxLen && in[c + l] == in[p + l]) l++;
                if (l <= 3 && d > 0xFF) continue;
                if (l > best) {
                    best = l;
                    bestDist = d;
                    if (l == maxLen) break;
                }
            }
        }
        if (best < 2) {
            for (int d = 1; d <= 0xFF && d <= p; d++) {
                if (in[p - d] == in[p] && in[p - d + 1] == in[p + 1]) {
                    best = 2;
                    bestDist = d;
                    if (maxLen > 2 && in[p - d + 2] == in[p + 2]) best = 3;
                    break;
                }
            }
        }
        return best;
    };

    int state = 0, pos = 0;
    uint8_t last = 0;
    auto emitLiteral = [&](int p) {
        e.bit(&m->match[state][0], 0);
        if (state > 0) state--;
        e.bittree(m->literal[(last >> kLc) & 7], 0x100, in[p]);
    };
    auto emitMatch = [&](int p, int len, int dist) {  // len = bytes to copy (2..255) or 0x100 for end
        int number = len - 1;
        e.bit(&m->match[state][0], 1);
        int n = log2i(number);
        for (int j = 0; j < n; j++) e.bit(&m->match[state][1 + j], 1);
        if (n < 7) e.bit(&m->match[state][1 + n], 0);
        if (n > 0) e.numberBits(&m->len[state][(n - 1) * 4 + ((p << (n - 1)) & 3)], n, number);
        if (number == 0xFF) return;
        int limit = number > 2 ? 0x2C : 8;
        int db = log2i(dist);
        e.bittree(&m->distBits[n][number > 2 ? 7 : 0], limit, db);
        if (db > 0) e.numberBits(m->dist[db], db, dist);
        state = ((p + len + 1) & 1) + 6;
    };

    while (pos < inLen) {
        int dist;
        int len = findMatch(pos, dist);
        if (len >= 2 && len < kMaxMatch && pos + 1 < inLen) {
            // one-step lazy evaluation
            insert(pos);
            int dist2;
            int len2 = findMatch(pos + 1, dist2);
            if (len2 > len + 1) {
                emitLiteral(pos);
                last = in[pos];
                pos++;
                len = len2;
                dist = dist2;
                for (int k = 1; k < len; k++) insert(pos + k);
                insert(pos);  // (pos itself inserted below via loop start? keep simple)
                emitMatch(pos, len, dist);
                pos += len;
                last = in[pos - 1];
                if (e.overflow) return -1;
                continue;
            }
            for (int k = 1; k < len; k++) insert(pos + k);
            emitMatch(pos, len, dist);
            pos += len;
            last = in[pos - 1];
        } else if (len >= 2) {
            for (int k = 0; k < len; k++) insert(pos + k);
            emitMatch(pos, len, dist);
            pos += len;
            last = in[pos - 1];
        } else {
            insert(pos);
            emitLiteral(pos);
            last = in[pos];
            pos++;
        }
        if (e.overflow) return -1;
    }
    emitMatch(pos, 0x100, 0);  // end marker
    e.flush();
    return e.overflow ? -1 : e.pos;
}

}  // namespace psp
}  // namespace ipbp
