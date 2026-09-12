#ifndef LZ4_BLOCK_H
#define LZ4_BLOCK_H

#include <cstdint>
#include <cstring>

static inline int lz4_compress_bound(int n) {
    return n + (n / 255) + 16;
}

static inline uint32_t lz4_read32(const void* p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

static inline uint64_t lz4_read64(const void* p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}

static int lz4_compress_block(const void* src, void* dst, int srcSize, int maxOut) {
    if (srcSize <= 0 || maxOut <= 0) return 0;

    const uint8_t* ip = (const uint8_t*)src;
    const uint8_t* const base = ip;
    const uint8_t* const iend = ip + srcSize;
    const uint8_t* const mflimit = iend - 12;
    const uint8_t* const matchlimit = iend - 5;
    const uint8_t* anchor = ip;

    uint8_t* op = (uint8_t*)dst;
    uint8_t* const olimit = op + maxOut;

    // 哈希表：per-thread 持久化，且不再每次压缩都清空。
    // 原来是 256KB 栈数组 + 每次 memset 256KB —— profile 实测 __memset_aarch64 独占 27% CPU
    // （每压一个块清一次表），这 256KB 栈帧也曾撑爆 clone/vfork 的子栈。
    // 不清表的安全前提：下面查表时校验候选偏移必须落在「本块内且严格在 ip 之前」，
    // 上一块残留的陈旧值会被挡掉，不会越界读；代价只是偶尔少几个匹配（实测压缩率 +0.017%）。
    // thread_local 静态数组由运行时零初始化一次，多线程各持一份。
    static thread_local uint32_t htab[1 << 16];

    auto hash4 = [](uint32_t v) -> uint32_t {
        return (v * 2654435761U) >> 16;
    };

    if (srcSize < 13) goto _last_literals;

    htab[hash4(lz4_read32(ip))] = 0;
    ip++;

    for (;;) {
        const uint8_t* ref;

        // find match (accelerated skip)
        {
            const uint8_t* fwd = ip;
            unsigned step = 1;
            unsigned searchLimit = 64;
            bool candOk;
            do {
                ip = fwd;
                fwd += step;
                step = (searchLimit++ >> 6);
                if (fwd > mflimit) goto _last_literals;
                uint32_t curOff = (uint32_t)(ip - base);
                uint32_t h = hash4(lz4_read32(ip));
                uint32_t cand = htab[h];
                htab[h] = curOff;
                // 只有落在本块内、且严格在 ip 之前的候选才可解引用（挡住陈旧值 → 无越界读）。
                // 表为全零时 cand=0 < curOff，ref=base，与原实现行为一致。
                candOk = (cand < curOff);
                ref = base + cand;
            } while (!candOk || lz4_read32(ref) != lz4_read32(ip) || (ip - ref) > 0xFFFF);
        }

        // encode literals
        {
            unsigned litLen = (unsigned)(ip - anchor);
            uint8_t* token = op++;
            if (op + litLen + (litLen / 255) + 16 > olimit) return 0;
            if (litLen >= 15) {
                *token = 0xF0;
                unsigned n = litLen - 15;
                for (; n >= 255; n -= 255) *op++ = 255;
                *op++ = (uint8_t)n;
            } else {
                *token = (uint8_t)(litLen << 4);
            }
            memcpy(op, anchor, litLen);
            op += litLen;

            // encode offset
            uint16_t offset = (uint16_t)(ip - ref);
            memcpy(op, &offset, 2);
            op += 2;

            // extend match forward (8-byte fast path)
            ip += 4; ref += 4;
            const uint8_t* sStart = ip;
            while (ip + 8 <= matchlimit) {
                uint64_t diff = lz4_read64(ip) ^ lz4_read64(ref);
                if (diff) { ip += __builtin_ctzll(diff) >> 3; goto _encode_ml; }
                ip += 8; ref += 8;
            }
            while (ip < matchlimit && *ip == *ref) { ip++; ref++; }

        _encode_ml:
            {
                unsigned ml = (unsigned)(ip - sStart);
                if (ml >= 15) {
                    *token |= 0x0F;
                    unsigned n = ml - 15;
                    if (op + (n / 255) + 2 > olimit) return 0;
                    for (; n >= 255; n -= 255) *op++ = 255;
                    *op++ = (uint8_t)n;
                } else {
                    *token |= (uint8_t)ml;
                }
            }

            anchor = ip;
        }

        if (ip >= mflimit) goto _last_literals;
        htab[hash4(lz4_read32(ip - 2))] = (uint32_t)(ip - 2 - base);
    }

_last_literals:
    {
        unsigned lastRun = (unsigned)(iend - anchor);
        if (op + lastRun + 1 + (lastRun / 255) > olimit) return 0;
        uint8_t* token = op++;
        if (lastRun >= 15) {
            *token = (15 << 4);
            unsigned n = lastRun - 15;
            for (; n >= 255; n -= 255) *op++ = 255;
            *op++ = (uint8_t)n;
        } else {
            *token = (uint8_t)(lastRun << 4);
        }
        memcpy(op, anchor, lastRun);
        op += lastRun;
    }

    return (int)(op - (uint8_t*)dst);
}

#endif // LZ4_BLOCK_H
