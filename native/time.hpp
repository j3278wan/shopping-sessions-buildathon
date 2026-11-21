#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace shopping_time {
using Text = std::string_view;
constexpr int64_t invalid_time = std::numeric_limits<int64_t>::min();
constexpr int64_t day_ms = 86400000;

// Keep one parser per worker (or thread_local), never share it between threads.
// Cached keys compare all ten original date bytes, including punctuation.
class Parser {
    struct DateEntry {
        uint64_t first = 0;
        uint16_t last = 0;
        int64_t milliseconds = invalid_time;
    };
    std::array<DateEntry, 64> dates_{};

    static inline int two(const char *p) {
        unsigned a = static_cast<unsigned char>(p[0]) - '0';
        unsigned b = static_cast<unsigned char>(p[1]) - '0';
        return (a < 10 && b < 10) ? int(10 * a + b) : -1;
    }

    static uint64_t word8(const char *p) {
        uint64_t v;
        std::memcpy(&v, p, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        v = __builtin_bswap64(v);
#endif
        return v;
    }
    static constexpr uint64_t nibbles = UINT64_C(0x0f0f0f0f0f0f0f0f);
    static constexpr uint64_t sixes = UINT64_C(0x0606060606060606);
    static constexpr uint64_t carry = UINT64_C(0x1010101010101010);
    static bool digits8(uint64_t x) {
        return (x & ~nibbles) == UINT64_C(0x3030303030303030) && !(((x & nibbles) + sixes) & carry);
    }
    // Decimal reductions on independent byte, 16-bit, and 32-bit lanes.
    static uint32_t decimal8(uint64_t digits) {
        auto pairs = (digits * 10 + (digits >> 8)) & UINT64_C(0x00ff00ff00ff00ff);
        auto quads = (pairs * 100 + (pairs >> 16)) & UINT64_C(0x0000ffff0000ffff);
        return uint32_t(quads) * 10000 + uint32_t(quads >> 32);
    }
    static int64_t epoch13(const char *p) {
        uint64_t first = word8(p), last = word8(p + 5);
        if (!digits8(first) || !digits8(last))
            return invalid_time;
        auto front = decimal8(first & nibbles);
        auto tail = decimal8(last & UINT64_C(0x0f0f0f0f0f000000));
        return int64_t(front) * 100000 + tail;
    }
    template <bool Checked> static int seconds(const char *p) {
        // HH:MM:SS is exactly eight bytes. Adding six to a decimal nibble
        // crosses bit four only for a non-decimal nibble; no cross-byte carry.
        uint64_t x = word8(p);
        auto digits = x & nibbles;
        if constexpr (Checked) {
            constexpr uint64_t colon = UINT64_C(0x0000ff0000ff0000);
            if ((x & ~nibbles) != UINT64_C(0x3030303030303030) ||
                (x & colon) != UINT64_C(0x00003a00003a0000) || ((digits + sixes) & carry & ~colon))
                return -1;
        }
        auto pairs = digits * 10 + (digits >> 8);
        int hour = pairs & 255, minute = (pairs >> 24) & 255, second = (pairs >> 48) & 255;
        if constexpr (Checked)
            if (hour >= 24 || minute >= 60 || second >= 60)
                return -1;
        return hour * 3600 + minute * 60 + second;
    }

    inline int64_t date(const char *p) {
        uint64_t first;
        uint16_t last;
        std::memcpy(&first, p, sizeof(first));
        std::memcpy(&last, p + 8, sizeof(last));
        auto &entry =
            dates_[(unsigned(static_cast<unsigned char>(p[8])) * 10 + static_cast<unsigned char>(p[9])) &
                   63];
        if (entry.first == first && entry.last == last)
            return entry.milliseconds;

        if (p[4] != '-' || p[7] != '-')
            return invalid_time;
        int y1 = two(p), y2 = two(p + 2), month = two(p + 5), day = two(p + 8);
        if (y1 < 0 || y2 < 0 || month <= 0 || month > 12 || day <= 0)
            return invalid_time;
        int year = y1 * 100 + y2;
        constexpr int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        if (year == 0 || day > month_days[month - 1] + (month == 2 && leap))
            return invalid_time;
        year -= month <= 2;
        int era = year / 400, y = year % 400;
        int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        int days = era * 146097 + y * 365 + y / 4 - y / 100 + doy - 719468;
        entry = {first, last, int64_t(days) * day_ms};
        return entry.milliseconds;
    }

  public:
    // Complete validation for event/click timestamps. No trimming or Unicode
    // digit acceptance; invalid or overflowing values return invalid_time.
    inline int64_t strict(Text ts) {
        if (ts.empty())
            return invalid_time;
        if (ts.size() == 13)
            return epoch13(ts.data());
        bool utc = ts.size() == 20 && ts[19] == 'Z';
        bool offset = ts.size() == 25 && (ts[19] == '+' || ts[19] == '-') && ts[22] == ':';
        if ((!utc && !offset) || ts[4] != '-') {
            uint64_t n = 0;
            auto [end, err] = std::from_chars(ts.data(), ts.data() + ts.size(), n);
            if (err != std::errc{} || end != ts.data() + ts.size() ||
                n > uint64_t(std::numeric_limits<int64_t>::max()))
                return invalid_time;
            return static_cast<int64_t>(n);
        }
        if (ts[10] != 'T')
            return invalid_time;
        const char *p = ts.data();
        int clock = seconds<true>(p + 11);
        if (clock < 0)
            return invalid_time;
        int off = 0;
        if (offset) {
            int oh = two(p + 20), om = two(p + 23);
            if (oh < 0 || oh >= 24 || om < 0 || om >= 60)
                return invalid_time;
            off = (60 * oh + om) * (p[19] == '-' ? -1 : 1);
        }
        int64_t midnight = date(p);
        if (midnight == invalid_time)
            return invalid_time;
        return midnight + (clock - off * 60) * 1000;
    }

    // Ingest timestamps have an exact F1 guarantee. On the common path two
    // UTC timestamps sharing fourteen bytes share the date and hour.
    inline int64_t event_pair(Text event, Text ingest, int64_t &ingest_ms) {
        if (event.size() != 20 || event[19] != 'Z' || ingest.size() != 20 || ingest[19] != 'Z' ||
            std::memcmp(ingest.data(), event.data(), 14) != 0) {
            ingest_ms = valid_f1_ingest(ingest);
            return strict(event);
        }
        auto midnight = date(ingest.data());
        if (midnight == invalid_time) {
            ingest_ms = invalid_time;
            return strict(event);
        }
        uint64_t a = word8(ingest.data() + 11) & nibbles;
        a = a * 10 + (a >> 8);
        int64_t hour = midnight + int64_t(a & 255) * 3600000;
        auto minute_seconds = [](uint64_t pairs) {
            return int((pairs >> 24) & 255) * 60 + int((pairs >> 48) & 255);
        };
        ingest_ms = hour + minute_seconds(a) * 1000;
        uint64_t x = word8(event.data() + 11), digits = x & nibbles;
        constexpr uint64_t decimal = UINT64_C(0xffff00ffff000000);
        if ((x & ~nibbles & decimal) != UINT64_C(0x3030003030000000) || ((x >> 40) & 255) != ':' ||
            ((digits + sixes) & carry & decimal))
            return invalid_time;
        uint64_t b = digits * 10 + (digits >> 8);
        if (((b >> 24) & 255) >= 60 || ((b >> 48) & 255) >= 60)
            return invalid_time;
        return hour + minute_seconds(b) * 1000;
    }

    // PRECONDITION: exactly a valid 20-byte F1 UTC timestamp. The published
    // contract guarantees this for ingest_ts, not for event.ts or click.ts.
    // This must not be called on arbitrary event/click timestamp strings.
    inline int64_t valid_f1_ingest(Text ts) {
        if (ts.size() != 20 || ts.back() != 'Z')
            return strict(ts);
        const char *p = ts.data();
        int clock = seconds<false>(p + 11);
        const auto midnight = date(p);
        if (midnight == invalid_time)
            return invalid_time;
        return midnight + clock * 1000;
    }
};
} // namespace shopping_time
