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
        if (ts[10] != 'T' || ts[13] != ':' || ts[16] != ':')
            return invalid_time;
        const char *p = ts.data();
        int h = two(p + 11), minute = two(p + 14), sec = two(p + 17);
        if (h < 0 || h >= 24 || minute < 0 || minute >= 60 || sec < 0 || sec >= 60)
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
        return midnight + ((h * 60 + minute - off) * 60 + sec) * 1000;
    }

    // PRECONDITION: exactly a valid 20-byte F1 UTC timestamp. The published
    // contract guarantees this for ingest_ts, not for event.ts or click.ts.
    // This must not be called on arbitrary event/click timestamp strings.
    inline int64_t valid_f1_ingest(Text ts) {
        if (ts.size() != 20 || ts.back() != 'Z')
            return strict(ts);
        const char *p = ts.data();
        int h = (p[11] - '0') * 10 + p[12] - '0';
        int minute = (p[14] - '0') * 10 + p[15] - '0';
        int sec = (p[17] - '0') * 10 + p[18] - '0';
        const auto midnight = date(p);
        if (midnight == invalid_time)
            return invalid_time;
        return midnight + ((h * 60 + minute) * 60 + sec) * 1000;
    }
};
} // namespace shopping_time
