#include "common.hpp"

#include <algorithm>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <parquet/arrow/writer.h>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static void require(arrow::Status result) {
    if (!result.ok())
        throw std::runtime_error(result.ToString());
}

template <typename T> T unwrap(arrow::Result<T> result) {
    if (!result.ok())
        throw std::runtime_error(result.status().ToString());
    return std::move(result).ValueUnsafe();
}

static void write_table(const fs::path &path, std::vector<std::string> columns,
                        std::vector<std::shared_ptr<arrow::Array>> arrays) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (size_t i = 0; i < columns.size(); ++i)
        fields.push_back(arrow::field(columns[i], arrays[i]->type()));
    auto table = arrow::Table::Make(arrow::schema(std::move(fields)), std::move(arrays));
    auto sink = unwrap(arrow::io::FileOutputStream::Open(path.string()));
    auto properties = parquet::WriterProperties::Builder()
                          .compression(parquet::Compression::SNAPPY)
                          ->disable_dictionary()
                          ->disable_statistics()
                          ->build();
    require(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), sink,
                                       std::max<int64_t>(table->num_rows(), 1), properties));
    require(sink->Close());
}

static void write_users(const fs::path &path, const std::vector<UserRow> &rows, const Input &in) {
    arrow::StringBuilder people, categories;
    arrow::Date32Builder days;
    arrow::Int64Builder sessions, events, revenue;
    for (arrow::ArrayBuilder *col :
         std::vector<arrow::ArrayBuilder *>{&people, &days, &sessions, &events, &revenue, &categories})
        require(col->Reserve(rows.size()));
    int64_t person_bytes = 0, category_bytes = 0;
    for (const auto &row : rows) {
        person_bytes += in.people[row.person].size();
        if (row.top_category >= 0)
            category_bytes += in.categories[row.top_category].size();
    }
    require(people.ReserveData(person_bytes));
    require(categories.ReserveData(category_bytes));
    for (const auto &row : rows) {
        people.UnsafeAppend(in.people[row.person]);
        days.UnsafeAppend(row.day);
        sessions.UnsafeAppend(row.sessions);
        events.UnsafeAppend(row.events);
        revenue.UnsafeAppend(row.revenue);
        if (row.top_category < 0)
            categories.UnsafeAppendNull();
        else
            categories.UnsafeAppend(in.categories[row.top_category]);
    }
    write_table(path, {"user_id", "day", "sessions", "events", "revenue_cents", "top_category"},
                {unwrap(people.Finish()), unwrap(days.Finish()), unwrap(sessions.Finish()),
                 unwrap(events.Finish()), unwrap(revenue.Finish()), unwrap(categories.Finish())});
}

static void write_campaigns(const fs::path &path, const std::vector<std::vector<CampaignRow>> &parts,
                            const Input &in) {
    std::unordered_map<uint64_t, CampaignRow> totals;
    for (const auto &part : parts) {
        for (const auto &row : part) {
            auto key = (uint64_t(row.campaign) << 32) | uint32_t(row.day);
            auto [it, inserted] = totals.try_emplace(key, row);
            if (!inserted) {
                auto &sum = it->second;
                sum.sessions += row.sessions;
                sum.purchasing += row.purchasing;
                sum.revenue += row.revenue;
                sum.assists += row.assists;
            }
        }
    }
    arrow::StringBuilder campaigns, channels;
    arrow::Date32Builder days;
    arrow::Int64Builder sessions, purchasing, revenue, assists;
    for (arrow::ArrayBuilder *col : std::vector<arrow::ArrayBuilder *>{
             &campaigns, &channels, &days, &sessions, &purchasing, &revenue, &assists})
        require(col->Reserve(totals.size()));
    for (const auto &item : totals) {
        const auto &row = item.second;
        require(campaigns.Append(in.campaigns[row.campaign]));
        require(channels.Append(in.channels[row.campaign]));
        days.UnsafeAppend(row.day);
        sessions.UnsafeAppend(row.sessions);
        purchasing.UnsafeAppend(row.purchasing);
        revenue.UnsafeAppend(row.revenue);
        assists.UnsafeAppend(row.assists);
    }
    write_table(path,
                {"campaign_id", "channel", "day", "sessions", "purchasing_sessions", "revenue_cents",
                 "assisted_sessions"},
                {unwrap(campaigns.Finish()), unwrap(channels.Finish()), unwrap(days.Finish()),
                 unwrap(sessions.Finish()), unwrap(purchasing.Finish()), unwrap(revenue.Finish()),
                 unwrap(assists.Finish())});
}

static int64_t manifest_time(const std::string &manifest, const std::string &key) {
    std::smatch match;
    std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]+)\"");
    if (!std::regex_search(manifest, match, pattern))
        throw std::runtime_error("Missing " + key + " in manifest.json");
    std::string value = match[1];
    std::tm utc{};
    auto end = strptime(value.c_str(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    if (!end || *end)
        throw std::runtime_error("Invalid manifest UTC time: " + value);
    return static_cast<int64_t>(timegm(&utc)) * 1000;
}

static void run() {
    const fs::path data = std::getenv("DATA_DIR") ? std::getenv("DATA_DIR") : "/data";
    const fs::path output = std::getenv("OUTPUT_DIR") ? std::getenv("OUTPUT_DIR") : "/output";
    int threads = std::getenv("NCPU") ? std::stoi(std::getenv("NCPU")) : 8;
    threads = std::max(1, threads);
    std::ifstream file(data / "manifest.json");
    if (!file)
        throw std::runtime_error("Cannot read input manifest");
    std::string manifest(std::istreambuf_iterator<char>{file}, {});
    auto started = Clock::now();
    auto input = ingest(data, threads, manifest_time(manifest, "window_start"),
                        manifest_time(manifest, "window_end"));
    auto loaded = Clock::now();
    fs::create_directories(output / "user_daily");
    fs::create_directories(output / "campaign_daily");
    std::vector<std::vector<CampaignRow>> campaigns(input.events.size());
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex mutex;
    std::exception_ptr error;
    std::vector<std::thread> pool;
    auto worker = [&] {
        try {
            while (!failed.load()) {
                auto part = next.fetch_add(1);
                if (part >= input.events.size())
                    break;
                auto reports = summarize(input.events[part], input.clicks[part], input);
                std::vector<Event>().swap(input.events[part]);
                std::vector<Click>().swap(input.clicks[part]);
                if (part == 0 || !reports.users.empty())
                    write_users(output / "user_daily" / ("part-" + std::to_string(part) + ".parquet"),
                                reports.users, input);
                campaigns[part] = std::move(reports.campaigns);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!error)
                error = std::current_exception();
            failed.store(true);
        }
    };
    int workers = std::min<size_t>(threads, input.events.size());
    for (int n = 1; n < workers; ++n)
        pool.emplace_back(worker);
    worker();
    for (auto &t : pool)
        t.join();
    if (error)
        std::rethrow_exception(error);
    if (input.events.empty())
        write_users(output / "user_daily" / "part-0.parquet", {}, input);
    write_campaigns(output / "campaign_daily" / "part-0.parquet", campaigns, input);
    if (std::getenv("SHOPPING_TIMINGS")) {
        std::cerr << "ingest " << std::chrono::duration<double>(loaded - started).count()
                  << " s, reports and Parquet "
                  << std::chrono::duration<double>(Clock::now() - loaded).count() << " s\n";
    }
}

int main() {
    try {
        run();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
