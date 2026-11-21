#include "common.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

constexpr int64_t session_gap_ms = 30 * 60 * 1000LL;
constexpr int64_t day_ms = 24 * 60 * 60 * 1000LL;
constexpr int64_t week_ms = 7 * day_ms;

int32_t utc_day(int64_t ts) {
    // Floor rather than truncate when a session starts before the Unix epoch.
    return static_cast<int32_t>(ts / day_ms - (ts % day_ms < 0));
}

// Group dense person IDs by counting first, then sort each user's shorter
// timeline. The same low ID bits are shared by all records of a hash partition.
template <class T, class WithinPerson>
void sort_by_person(std::vector<T> &rows, WithinPerson time_order) {
    auto full_order = [&](const T &a, const T &b) {
        return a.person == b.person ? time_order(a, b) : a.person < b.person;
    };
    if (rows.size() < 1024) {
        std::sort(rows.begin(), rows.end(), full_order);
        return;
    }
    const uint32_t sample = rows.front().person;
    uint32_t differing = 0, smallest = sample, largest = sample;
    for (const auto &row : rows) {
        differing |= row.person ^ sample;
        smallest = std::min(smallest, row.person);
        largest = std::max(largest, row.person);
    }
    if (!differing) {
        std::sort(rows.begin(), rows.end(), time_order);
        return;
    }
    const unsigned shift = __builtin_ctz(differing);
    auto bucket = [&](const T &row) { return (row.person - smallest) >> shift; };
    size_t number = size_t((largest - smallest) >> shift) + 1;
    if (number > rows.size() * 2) {
        std::sort(rows.begin(), rows.end(), full_order);
        return;
    }
    std::vector<size_t> ends(number, 0);
    for (const T &row : rows)
        ++ends[bucket(row)];
    size_t start = 0;
    for (auto &count : ends) {
        const size_t n = count;
        count = start;
        start += n;
    }
    std::vector<T> ordered(rows.size());
    for (const T &row : rows)
        ordered[ends[bucket(row)]++] = row;
    rows.swap(ordered);
    start = 0;
    for (size_t end : ends) {
        if (end > start + 1)
            std::sort(rows.begin() + start, rows.begin() + end, time_order);
        start = end;
    }
}

// Both ends of the eligible click window move forward with session start time.
// Keep only campaigns with a positive click count in active_, so each campaign
// gets exactly one assist per session, even for users with thousands of clicks.
class ActiveCampaigns {
  public:
    explicit ActiveCampaigns(size_t size) : counts_(size), positions_(size) {}

    void reset() {
        for (uint32_t campaign : active_)
            counts_[campaign] = 0;
        active_.clear();
    }

    void add(uint32_t campaign) {
        if (counts_[campaign]++ == 0) {
            positions_[campaign] = active_.size();
            active_.push_back(campaign);
        }
    }

    void remove(uint32_t campaign) {
        if (--counts_[campaign] == 0) {
            const auto slot = positions_[campaign];
            const auto replacement = active_.back();
            active_[slot] = replacement;
            positions_[replacement] = slot;
            active_.pop_back();
        }
    }

    void count_assists(std::vector<CampaignRow> &day) const {
        for (uint32_t campaign : active_)
            ++day[campaign].assists;
    }

  private:
    std::vector<uint32_t> counts_;
    std::vector<size_t> positions_;
    std::vector<uint32_t> active_;
};

} // namespace

Reports summarize(std::vector<Event> &events, std::vector<Click> &clicks, const Input &input) {
    Reports output;
    if (events.empty())
        return output;
    sort_by_person(events, [](const Event &a, const Event &b) { return a.ts < b.ts; });
    sort_by_person(clicks, [](const Click &a, const Click &b) {
        // The last eligible click will have the smallest ID at a tied time.
        return a.ts == b.ts ? a.id > b.id : a.ts < b.ts;
    });

    output.users.reserve(events.size() / 16);
    ActiveCampaigns active(input.campaigns.size());
    std::unordered_map<int32_t, std::vector<CampaignRow>> campaigns_by_day;
    std::vector<int64_t> category_revenue(input.categories.size(), 0);
    std::vector<uint32_t> purchased_categories;

    size_t event = 0;
    size_t right_click = 0;
    while (event < events.size()) {
        const uint32_t person = events[event].person;
        while (right_click < clicks.size() && clicks[right_click].person < person)
            ++right_click;
        size_t left_click = right_click;
        active.reset();

        do {
            const int64_t start = events[event].ts;
            const int32_t day = utc_day(start);
            if (output.users.empty() || output.users.back().person != person ||
                output.users.back().day != day) {
                for (uint32_t c : purchased_categories)
                    category_revenue[c] = 0;
                purchased_categories.clear();
                output.users.push_back(UserRow{person, day, 0, 0, 0, -1});
            }
            UserRow &user = output.users.back();
            ++user.sessions;
            int64_t session_revenue = 0;
            const size_t first_event = event;
            do {
                const Event &current = events[event++];
                if (current.cents > 0) {
                    session_revenue += current.cents;
                    const uint32_t category = current.category;
                    int64_t &total = category_revenue[category];
                    if (total == 0)
                        purchased_categories.push_back(category);
                    total += current.cents;
                    const int32_t best = user.top_category;
                    if (best < 0 || total > category_revenue[best] ||
                        (total == category_revenue[best] &&
                         input.categories[category] < input.categories[best])) {
                        user.top_category = static_cast<int32_t>(category);
                    }
                }
            } while (event < events.size() && events[event].person == person &&
                     events[event].ts - events[event - 1].ts <= session_gap_ms);
            user.events += static_cast<int64_t>(event - first_event);
            user.revenue += session_revenue;

            while (right_click < clicks.size() && clicks[right_click].person == person &&
                   clicks[right_click].ts <= start) {
                active.add(clicks[right_click++].campaign);
            }
            while (left_click < right_click && clicks[left_click].ts < start - week_ms) {
                active.remove(clicks[left_click++].campaign);
            }
            auto &by_campaign = campaigns_by_day.try_emplace(day, input.campaigns.size()).first->second;
            const uint32_t credit = right_click > left_click ? clicks[right_click - 1].campaign : 0;
            CampaignRow &row = by_campaign[credit];
            ++row.sessions;
            row.purchasing += (session_revenue > 0);
            row.revenue += session_revenue;
            active.count_assists(by_campaign);
        } while (event < events.size() && events[event].person == person);
    }
    for (auto &[day, rows] : campaigns_by_day) {
        for (size_t i = 0; i < rows.size(); ++i) {
            CampaignRow &row = rows[i];
            if (row.sessions || row.assists) {
                row.campaign = static_cast<uint32_t>(i);
                row.day = day;
                output.campaigns.push_back(row);
            }
        }
    }
    return output;
}
