//
// RT64
//

#pragma once

#include <cstring>

// Referenced from zstd.

static unsigned bitScanForward64(uint64_t val) {
    assert(val != 0);

#if defined(_MSC_VER) && defined(_WIN64)
#if STATIC_BMI2 == 1
    return (unsigned)_tzcnt_u64(val);
#else
    if (val != 0) {
        unsigned long r;
        _BitScanForward64(&r, val);
        return (unsigned)r;
    }
    else {
        /* Should not reach this code path */
        __assume(0);
    }
#endif
#elif defined(__GNUC__) && (__GNUC__ >= 4) && (defined(__LP64__) || defined(__wasm__))
    return (unsigned)__builtin_ctzll(val);
#elif defined(__ICCARM__)
    return (unsigned)__builtin_ctzll(val);
#else
    static_assert(false, "bitScanForward64 not implemented for this compiler.");
#endif
}

namespace RT64 {
    struct TMEMHasher {
        static const uint32_t CurrentHashVersion = 5;

        static bool needsToHashRowsIndividually(const LoadTile &loadTile, uint32_t width) {
            // When using 32-bit formats, TMEM contents are split in half in the lower and upper half, so the size per row is effectively
            // the same as a 16-bit format as far as TMEM is concerned.
            const bool RGBA32 = (loadTile.siz == G_IM_SIZ_32b) && (loadTile.fmt == G_IM_FMT_RGBA);
            uint32_t drawBytesPerRow = std::max(width << (RGBA32 ? G_IM_SIZ_16b : loadTile.siz) >> 1U, 1U);
            uint32_t tmemBytesPerRow = loadTile.line << 3;
            return tmemBytesPerRow > drawBytesPerRow;
        }

        // Collects the bytes a hash covers into one buffer and hashes them in one go. XXH3 gives the same hash for the same
        // bytes whether they come at once or piece by piece, and this skips the cost of every small streaming update.
        struct GatheredXXH3 {
            alignas(16) uint8_t buffer[8192];
            uint32_t size = 0;
            bool streaming = false;
            XXH3_state_t state;

            void update(const void *data, uint32_t byteCount) {
                if ((size + byteCount) > sizeof(buffer)) {
                    if (!streaming) {
                        XXH3_64bits_reset(&state);
                        streaming = true;
                    }

                    XXH3_64bits_update(&state, buffer, size);
                    size = 0;
                    if (byteCount > sizeof(buffer)) {
                        XXH3_64bits_update(&state, data, byteCount);
                        return;
                    }
                }

                memcpy(&buffer[size], data, byteCount);
                size += byteCount;
            }

            uint64_t digest() {
                if (!streaming) {
                    return XXH3_64bits(buffer, size);
                }

                XXH3_64bits_update(&state, buffer, size);
                return XXH3_64bits_digest(&state);
            }
        };

        static uint64_t hash(const uint8_t *TMEM, const LoadTile &loadTile, uint16_t width, uint16_t height, uint32_t tlut, uint32_t version) {
            const uint32_t TMEMBytes = 4096;
            const uint32_t TMEMMask8 = 4095;
            const uint32_t TMEMMask16 = 2047;
            GatheredXXH3 xxh3;
            const bool RGBA32 = (loadTile.siz == G_IM_SIZ_32b) && (loadTile.fmt == G_IM_FMT_RGBA);
            const bool usesTLUT = tlut > 0;
            bool halfTMEM = RGBA32;
            
            // Version 3 fixes an error where using TLUT did not mask the address to the lower half of TMEM.
            if ((version >= 3) && usesTLUT) {
                halfTMEM = true;
            }

            const uint32_t tmemSize = halfTMEM ? (TMEMBytes >> 1) : TMEMBytes;
            const uint32_t drawBytesPerRow = std::max(uint32_t(width) << (RGBA32 ? G_IM_SIZ_16b : loadTile.siz) >> 1U, 1U);
            const uint32_t drawBytesTotal = (loadTile.line << 3) * (height - 1) + drawBytesPerRow;
            const uint32_t tmemMask = halfTMEM ? TMEMMask16 : TMEMMask8;
            const uint32_t tmemAddress = (loadTile.tmem << 3) & tmemMask;
            uint64_t tlutBitset[4] = {};

            // Which byte values the texture uses, one byte per value: setting a flag is a plain store, where setting a bit
            // in tlutBitset waits on the update before it. Turned into tlutBitset once all of it is hashed.
            alignas(16) uint8_t tlutSeen[256];
            bool tlutSeenUsed = false;
            auto hashUpdate = [&](const uint8_t *tmemBytes, uint32_t byteCount) {
                xxh3.update(tmemBytes, byteCount);

                // Version 5 parses every byte individually if TLUT is enabled to determine the TLUT bytes that can be hashed.
                if ((version >= 5) && usesTLUT) {
                    if (!tlutSeenUsed) {
                        memset(tlutSeen, 0, sizeof(tlutSeen));
                        tlutSeenUsed = true;
                    }

                    for (uint32_t i = 0; i < byteCount; i++) {
                        tlutSeen[tmemBytes[i]] = 1;
                    }
                }
            };

            auto hashTMEM = [&](uint32_t tmemBaseAddress, uint32_t tmemOrAddress, uint32_t byteCount, bool oddRow) {
                assert((tmemBaseAddress < tmemSize) && "Base address must be masked within the bounds of the TMEM size.");

                // Too many bytes to hash in a single step. Wrap around TMEM and hash the rest.
                if ((tmemBaseAddress + byteCount) > tmemSize) {
                    const uint32_t firstBytes = (tmemSize - tmemBaseAddress);
                    hashUpdate(&TMEM[tmemBaseAddress | tmemOrAddress], firstBytes);

                    // Start hashing from the start of TMEM.
                    byteCount = std::min(byteCount - firstBytes, tmemBaseAddress);
                    tmemBaseAddress = 0;
                }

                // Version 4 fixes an error where odd row word swapping was not considered when hashing rows individually.
                if (oddRow && (version >= 4)) {
                    uint32_t wordCount = byteCount / 8;
                    if (wordCount > 0) {
                        hashUpdate(&TMEM[tmemBaseAddress | tmemOrAddress], wordCount * 8);
                        tmemBaseAddress += wordCount * 8;
                        byteCount -= wordCount * 8;
                    }
                    
                    if (byteCount > 4) {
                        hashUpdate(&TMEM[tmemBaseAddress | tmemOrAddress], byteCount - 4);
                        hashUpdate(&TMEM[(tmemBaseAddress + 4) | tmemOrAddress], 4);
                    }
                    else if (byteCount > 0) {
                        hashUpdate(&TMEM[(tmemBaseAddress + 4) | tmemOrAddress], byteCount);
                    }
                }
                else {
                    hashUpdate(&TMEM[tmemBaseAddress | tmemOrAddress], byteCount);
                }
            };

            // Version 2 introduces the ability to hash row by row because not all bytes of the line in TMEM are used.
            if ((version >= 2) && TMEMHasher::needsToHashRowsIndividually(loadTile, width)) {
                uint32_t tmemBytesPerRow = loadTile.line << 3;
                for (uint32_t i = 0; i < height; i++) {
                    hashTMEM((tmemAddress + i * tmemBytesPerRow) & tmemMask, 0x0, drawBytesPerRow, i & 1);
                }

                if (RGBA32) {
                    for (uint32_t i = 0; i < height; i++) {
                        hashTMEM((tmemAddress + i * tmemBytesPerRow) & tmemMask, tmemSize, drawBytesPerRow, i & 1);
                    }
                }
            }
            else {
                hashTMEM(tmemAddress, 0x0, drawBytesTotal, false);

                if (RGBA32) {
                    hashTMEM(tmemAddress, tmemSize, drawBytesTotal, false);
                }
            }

            // The byte values seen, as a bitset of the palette indices used: the byte values for 8-bit textures, and both
            // nibbles of each for 4-bit ones.
            if (tlutSeenUsed) {
                uint64_t byteBitset[4] = {};
                for (uint32_t w = 0; w < 4; w++) {
                    for (uint32_t b = 0; b < 64; b += 8) {
                        // Eight flags of 0 or 1, gathered into the top byte of the product, flag k at bit k.
                        uint64_t flags;
                        memcpy(&flags, &tlutSeen[w * 64 + b], sizeof(flags));
                        byteBitset[w] |= ((flags * 0x0102040810204080ULL) >> 56) << b;
                    }
                }

                if (loadTile.siz == G_IM_SIZ_4b) {
                    // Byte v uses indices v & 0xF and v >> 4: the low ones are the OR of the set's 16-bit pieces, and
                    // high index h is used when piece h (bytes h * 16 to h * 16 + 15) isn't empty.
                    uint64_t nibbles = 0;
                    for (uint32_t h = 0; h < 16; h++) {
                        const uint64_t piece = (byteBitset[h >> 2] >> ((h & 3) * 16)) & 0xFFFFU;
                        nibbles |= piece | (uint64_t(piece != 0) << h);
                    }
                    tlutBitset[0] |= nibbles;
                }
                else {
                    memcpy(tlutBitset, byteBitset, sizeof(tlutBitset));
                }
            }

            // If TLUT is active, we also hash the corresponding palette bytes.
            if (usesTLUT) {
                const bool CI4 = (loadTile.siz == G_IM_SIZ_4b);
                const int32_t paletteOffset = CI4 ? (loadTile.palette << 7) : 0;
                const int32_t paletteAddress = (TMEMBytes >> 1) + paletteOffset;

                // Version 5 stores a bitset of all the indices that should be hashed.
                if (version >= 5) {
                    // Fast path for a full bitset for CI4.
                    if (CI4 && (tlutBitset[0] == UINT16_MAX)) {
                        xxh3.update(&TMEM[paletteAddress], 0x80);
                    }
                    else {
                        const uint32_t bitsetCount = CI4 ? 1 : 4;
                        uint32_t bitsetIndex = 0;
                        uint32_t paletteIndex = 0;
                        for (uint32_t i = 0; i < bitsetCount; i++) {
                            // Fast path for a full bitset.
                            if (tlutBitset[i] == UINT64_MAX) {
                                xxh3.update(&TMEM[paletteAddress + i * 0x200], 0x200);
                            }
                            else {
                                // Each run of used indices in one update, as their palette bytes are side by side, and a lone
                                // one as 8 bytes.
                                uint64_t bits = tlutBitset[i];
                                while (bits > 0) {
                                    bitsetIndex = bitScanForward64(bits);
                                    paletteIndex = (i * 0x40) + bitsetIndex;
                                    if (((bits >> bitsetIndex) & 2) == 0) {
                                        xxh3.update(&TMEM[paletteAddress + paletteIndex * 8], 8);
                                        bits &= bits - 1;
                                        continue;
                                    }

                                    const uint64_t above = ~(bits >> bitsetIndex);
                                    const uint32_t runLength = (above == 0) ? (64 - bitsetIndex) : bitScanForward64(above);
                                    xxh3.update(&TMEM[paletteAddress + paletteIndex * 8], runLength * 8);
                                    bits = ((bitsetIndex + runLength) >= 64) ? 0 : (bits & (~0ULL << (bitsetIndex + runLength)));
                                }
                            }
                        }
                    }
                }
                else {
                    const int32_t bytesToHash = CI4 ? 0x80 : 0x800;
                    xxh3.update(&TMEM[paletteAddress], bytesToHash);
                }
            }
            
            // Encode more parameters into the hash that affect the final RGBA32 output.
            static_assert(sizeof(width) == 2, "Hash must use 16-bit width.");
            static_assert(sizeof(height) == 2, "Hash must use 16-bit height.");
            static_assert(sizeof(tlut) == 4, "Hash must use 32-bit TLUT.");
            static_assert(sizeof(loadTile.line) == 2, "Hash must use 16-bit line.");
            static_assert(sizeof(loadTile.siz) == 1, "Hash must use 8-bit siz.");
            static_assert(sizeof(loadTile.fmt) == 1, "Hash must use 8-bit fmt.");
            xxh3.update(&width, sizeof(width));
            xxh3.update(&height, sizeof(height));
            xxh3.update(&tlut, sizeof(tlut));
            xxh3.update(&loadTile.line, sizeof(loadTile.line));
            xxh3.update(&loadTile.siz, sizeof(loadTile.siz));
            xxh3.update(&loadTile.fmt, sizeof(loadTile.fmt));

            return xxh3.digest();
        }

        static bool requiresRawTMEM(const LoadTile &loadTile, uint16_t width, uint16_t height, uint32_t tlutFormat) {
            const uint32_t TMEMBytes = 4096;
            const bool RGBA32 = (loadTile.siz == G_IM_SIZ_32b) && (loadTile.fmt == G_IM_FMT_RGBA);
            const uint32_t tmemSize = RGBA32 || (tlutFormat > 0) ? (TMEMBytes >> 1) : TMEMBytes;
            const uint32_t lastRowBytes = width << std::min(loadTile.siz, uint8_t(G_IM_SIZ_16b)) >> 1;
            const uint32_t bytesToHash = (loadTile.line << 3) * (height - 1) + lastRowBytes;
            return (bytesToHash > tmemSize);
        }
    };
};