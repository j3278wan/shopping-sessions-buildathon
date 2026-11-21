#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

struct Event {
    uint32_t person;
    int64_t ts;
    uint32_t category;
    int32_t cents;
};

struct Click {
    int64_t ts;
    std::string_view id;
    uint32_t person;
    uint32_t campaign;

    Click() = default;
    Click(uint32_t person_, int64_t ts_, uint32_t campaign_, std::string_view id_)
        : ts(ts_), id(id_), person(person_), campaign(campaign_) {}
};
static_assert(sizeof(Click) == 32, "Click records must not contain avoidable padding");

struct Input {
    std::vector<std::string_view> people, categories, campaigns, channels;
    std::vector<std::vector<Event>> events;
    std::vector<std::vector<Click>> clicks;
    std::shared_ptr<void> storage;
};

struct UserRow {
    uint32_t person;
    int32_t day;
    int64_t sessions, events, revenue;
    int32_t top_category;
};
struct CampaignRow {
    uint32_t campaign;
    int32_t day;
    int64_t sessions, purchasing, revenue, assists;
};
struct Reports {
    std::vector<UserRow> users;
    std::vector<CampaignRow> campaigns;
};

Input ingest(const std::filesystem::path &data, int threads, int64_t start_ms, int64_t end_ms);
Reports summarize(std::vector<Event> &events, std::vector<Click> &clicks, const Input &input);
