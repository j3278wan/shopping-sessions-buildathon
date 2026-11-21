#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// The input dialect forbids embedded commas, quotes and newlines. Collect the
// commas in parallel and count all quotes. A valid quoted field accounts for
// exactly its opening and closing quotes, so no second scan of its contents is needed.
template <size_t N> bool split_fields(std::string_view line, std::array<std::string_view, N> &fields) {
    size_t cuts[N];
    size_t count = 0, quotes = 0, pos = 0;
    const char *data = line.data();
#if defined(__AVX2__)
    const auto comma = _mm256_set1_epi8(','), quote = _mm256_set1_epi8('"');
    for (; pos + 32 <= line.size(); pos += 32) {
        auto bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(data + pos));
        uint32_t mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, comma));
        quotes += __builtin_popcount(
            static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(bytes, quote))));
        while (mask) {
            if (count == N - 1)
                return false;
            cuts[count++] = pos + __builtin_ctz(mask);
            mask &= mask - 1;
        }
    }
#elif defined(__aarch64__) && defined(__ARM_NEON)
    const auto comma = vdupq_n_u8(','), quote = vdupq_n_u8('"');
    for (; pos + 16 <= line.size(); pos += 16) {
        auto bytes = vld1q_u8(reinterpret_cast<const uint8_t *>(data + pos));
        auto matches = vceqq_u8(bytes, comma);
        // Narrow each pair into one byte, leaving four bits per input position.
        uint64_t mask =
            vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(matches), 4)), 0) &
            0x1111111111111111ULL;
        quotes += vaddvq_u8(vshrq_n_u8(vceqq_u8(bytes, quote), 7));
        while (mask) {
            if (count == N - 1)
                return false;
            cuts[count++] = pos + __builtin_ctzll(mask) / 4;
            mask &= mask - 1;
        }
    }
#endif
    for (; pos < line.size(); ++pos) {
        if (data[pos] == ',') {
            if (count == N - 1)
                return false;
            cuts[count++] = pos;
        }
        quotes += data[pos] == '"';
    }
    if (count != N - 1)
        return false;
    cuts[count] = line.size();
    size_t start = 0, accounted = 0;
    for (size_t i = 0; i < N; ++i) {
        size_t length = cuts[i] - start;
        if (length && data[start] == '"') {
            if (length < 2 || data[cuts[i] - 1] != '"')
                return false;
            fields[i] = line.substr(start + 1, length - 2);
            accounted += 2;
        } else {
            fields[i] = line.substr(start, length);
        }
        start = cuts[i] + 1;
    }
    return accounted == quotes;
}
