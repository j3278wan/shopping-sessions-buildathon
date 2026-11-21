#include "common.hpp"
#include "csv.hpp"
#include "time.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using Text = std::string_view;
namespace fs = std::filesystem;
constexpr uint32_t NONE = std::numeric_limits<uint32_t>::max();
constexpr int64_t INVALID_TIME = std::numeric_limits<int64_t>::min();
constexpr int64_t DAY = 86400000;
using Clock = std::chrono::steady_clock;

struct Mappings {
    std::vector<std::pair<const char *, size_t>> files;
    ~Mappings() {
        for (auto [data, size] : files)
            munmap(const_cast<char *>(data), size);
    }
    Text open(const fs::path &path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::runtime_error("cannot read " + path.string());
        struct stat st{};
        if (fstat(fd, &st) || st.st_size < 0) {
            close(fd);
            throw std::runtime_error("cannot stat " + path.string());
        }
        if (!st.st_size) {
            close(fd);
            return {};
        }
        size_t len = static_cast<size_t>(st.st_size);
        void *p = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED)
            throw std::runtime_error("mmap failed: " + path.string());
        files.emplace_back(static_cast<const char *>(p), len);
        return {static_cast<const char *>(p), len};
    }
};

std::vector<Text> csv_parts(Mappings &maps, const fs::path &dir) {
    std::vector<fs::path> names;
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".csv")
            names.push_back(entry.path());
    }
    std::sort(names.begin(), names.end());
    std::vector<Text> data;
    for (const auto &name : names)
        data.push_back(maps.open(name));
    return data;
}

Text after_header(Text file) {
    auto at = file.find('\n');
    return at == Text::npos ? Text{} : file.substr(at + 1);
}

template <class Callback> void lines(Text input, Callback &&callback) {
    if (input.empty())
        return;
    const char *p = input.data();
    const char *end = p + input.size();
    while (p < end) {
        auto *lf = static_cast<const char *>(std::memchr(p, '\n', end - p));
        const char *stop = lf ? lf : end;
        callback(Text(p, static_cast<size_t>(stop - p)));
        if (!lf)
            break;
        p = lf + 1;
    }
}

// Inline short keys in their hash slots. For 8–15 bytes, the first eight and
// last seven bytes cover the entire original string, and the length prevents
// cross-length matches. Longer keys retain an exact, mmap-backed string view.
struct Key {
    uint64_t first = 0, last = 0;
    static constexpr uint64_t external = uint64_t{1} << 63;
    Key() = default;
    explicit Key(Text text) {
        size_t n = text.size();
        if (n > 15) {
            first = reinterpret_cast<uintptr_t>(text.data());
            last = external | n;
        } else {
            last = uint64_t(n + 1) << 56;
            if (n >= 8) {
                uint64_t tail;
                std::memcpy(&first, text.data(), 8);
                std::memcpy(&tail, text.data() + n - 8, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                tail = __builtin_bswap64(tail);
#endif
                last |= tail >> 8;
            } else if (n) {
                std::memcpy(&first, text.data(), n);
            }
        }
    }
    bool empty() const {
        return last == 0;
    }
    Text view() const {
        return {reinterpret_cast<const char *>(static_cast<uintptr_t>(first)),
                static_cast<size_t>(last & ~external)};
    }
    bool operator==(const Key &other) const {
        if (last != other.last)
            return false;
        if (first == other.first)
            return true;
        return (last & external) && view() == other.view();
    }
    bool operator!=(const Key &other) const {
        return !(*this == other);
    }
    size_t hash() const {
        if (last & external)
            return std::hash<Text>{}(view());
        uint64_t h = (first ^ (last * UINT64_C(0x9e3779b97f4a7c15))) * UINT64_C(0xbf58476d1ce4e5b9);
        h ^= h >> 30;
        h *= UINT64_C(0x94d049bb133111eb);
        return h ^ (h >> 31);
    }
};

template <class Value = uint32_t> class Index {
    struct Slot {
        Key key{};
        Value value{};
    };
    std::vector<Slot> table;
    size_t used = 0;
    size_t position(const Key &key, size_t hash) const {
        auto mask = table.size() - 1;
        size_t pos = hash & mask;
        while (!table[pos].key.empty() && table[pos].key != key)
            pos = (pos + 1) & mask;
        return pos;
    }
    void grow() {
        auto old = std::move(table);
        table.resize(std::max<size_t>(16, old.size() * 2));
        for (auto &s : old)
            if (!s.key.empty())
                table[position(s.key, s.key.hash())] = s;
    }

  public:
    const Value *find(const Key &key, size_t hash) const {
        if (table.empty())
            return nullptr;
        const auto &s = table[position(key, hash)];
        return s.key.empty() ? nullptr : &s.value;
    }
    const Value *find(Text text) const {
        Key key(text);
        return find(key, key.hash());
    }
    template <class Create> Value get_or_insert(const Key &key, size_t hash, Create create) {
        if (table.empty() || (used + 1) * 10 > table.size() * 7)
            grow();
        auto &s = table[position(key, hash)];
        if (s.key.empty()) {
            s.key = key;
            s.value = create();
            ++used;
        }
        return s.value;
    }
    template <class Create> Value add(Text text, Create create) {
        Key key(text);
        return get_or_insert(key, key.hash(), create);
    }
    template <class Func> void each_value(Func func) {
        for (auto &s : table)
            if (!s.key.empty())
                func(s.value);
    }
};

uint32_t intern(Index<> &index, std::vector<Text> &names, Text key) {
    return index.add(key, [&] {
        auto n = static_cast<uint32_t>(names.size());
        names.push_back(key);
        return n;
    });
}

class People {
    Index<> linked;
    std::vector<Text> &names;
    std::vector<uint32_t> parents;
    struct Shard {
        std::mutex mutex;
        Index<> new_ids;
        std::vector<std::pair<Text, uint32_t>> names;
    };
    std::array<Shard, 64> added;
    std::atomic<uint32_t> next{0};

    uint32_t link_id(Text key) {
        return linked.add(key, [&] {
            uint32_t id = static_cast<uint32_t>(names.size());
            names.push_back(key);
            parents.push_back(id);
            return id;
        });
    }
    uint32_t root(uint32_t i) {
        while (i != parents[i]) {
            parents[i] = parents[parents[i]];
            i = parents[i];
        }
        return i;
    }

  public:
    explicit People(std::vector<Text> &output) : names(output) {}
    void prepare(const std::vector<Text> &parts) {
        std::array<Text, 2> fields;
        for (Text part : parts)
            lines(after_header(part), [&](Text line) {
                if (!split_fields(line, fields) || fields[0].empty() || fields[1].empty())
                    return;
                uint32_t a = root(link_id(fields[0])), b = root(link_id(fields[1]));
                if (a == b)
                    return;
                if (names[b] < names[a])
                    std::swap(a, b);
                parents[b] = a;
            });
        linked.each_value([&](uint32_t &id) { id = root(id); });
        std::vector<uint32_t>().swap(parents);
        next.store(static_cast<uint32_t>(names.size()));
    }
    uint32_t lookup(Text name, const Key &key, size_t hash) {
        if (auto p = linked.find(key, hash))
            return *p;
        auto &s = added[(hash >> 16) % added.size()];
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.new_ids.get_or_insert(key, hash, [&] {
            uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
            s.names.emplace_back(name, id);
            return id;
        });
    }
    void finish() {
        names.resize(next.load());
        for (auto &s : added)
            for (auto [name, id] : s.names)
                names[id] = name;
    }
};

struct UserCache {
    shopping_time::Parser times;
    struct Cached {
        Key name;
        uint32_t id;
    };
    std::vector<Cached> slots = std::vector<Cached>(16384);
    uint32_t operator()(People &people, Text name) {
        Key key(name);
        size_t hash = key.hash();
        auto &hit = slots[hash & (slots.size() - 1)];
        if (hit.name != key)
            hit = {key, people.lookup(name, key, hash)};
        return hit.id;
    }
};

int32_t price(Text field) {
    if (field.empty())
        return 0;
    unsigned price = 0;
    auto [end, err] = std::from_chars(field.data(), field.data() + field.size(), price);
    return err == std::errc{} && end == field.data() + field.size() && price <= 10000000 ? int32_t(price)
                                                                                         : 0;
}

template <class Func> void parallel(size_t count, int workers, Func func) {
    size_t n = std::min(count, static_cast<size_t>(workers));
    if (!n)
        return;
    std::atomic<size_t> next{0};
    std::mutex mutex;
    std::exception_ptr error;
    auto run = [&](size_t worker) {
        try {
            for (;;) {
                size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= count)
                    break;
                func(worker, i);
            }
        } catch (...) {
            next.store(count);
            std::lock_guard<std::mutex> lock(mutex);
            if (!error)
                error = std::current_exception();
        }
    };
    std::vector<std::thread> threads;
    for (size_t w = 1; w < n; ++w)
        threads.emplace_back(run, w);
    run(0);
    for (auto &t : threads)
        t.join();
    if (error)
        std::rethrow_exception(error);
}

std::vector<Text> chunks(const std::vector<Text> &parts) {
    std::vector<Text> result;
    constexpr size_t block = 512 * 1024;
    for (Text file : parts) {
        Text body = after_header(file);
        while (!body.empty()) {
            auto lf = body.size() > block ? body.find('\n', block) : Text::npos;
            size_t len = lf == Text::npos ? body.size() : lf + 1;
            result.push_back(body.substr(0, len));
            body.remove_prefix(len);
        }
    }
    return result;
}

struct Catalog {
    Index<> products, campaigns;
    uint32_t unknown;
    void load(Input &in, Mappings &files, const fs::path &data) {
        Index<> categories;
        unknown = intern(categories, in.categories, "unknown");
        std::array<Text, 2> fields;
        lines(after_header(files.open(data / "products.csv")), [&](Text line) {
            if (!split_fields(line, fields) || fields[0].empty())
                return;
            uint32_t c = intern(categories, in.categories, fields[1]);
            products.add(fields[0], [&] { return c; });
        });
        in.campaigns.push_back("organic");
        in.channels.push_back("organic");
        lines(after_header(files.open(data / "campaigns.csv")), [&](Text line) {
            if (!split_fields(line, fields) || fields[0].empty())
                return;
            campaigns.add(fields[0], [&] {
                uint32_t i = static_cast<uint32_t>(in.campaigns.size());
                in.campaigns.push_back(fields[0]);
                in.channels.push_back(fields[1]);
                return i;
            });
        });
    }
};

struct EventCandidate {
    Key id;
    int64_t ingest, ts;
    uint32_t person, category;
    int32_t cents;
    uint32_t partition_hash; // Hash bits left after selecting the partition.
};

template <class Output> using WorkerBuckets = std::vector<std::vector<std::vector<Output>>>;

template <class Output>
void merge_people(WorkerBuckets<Output> &sources, int threads, std::vector<std::vector<Output>> &out) {
    parallel(out.size(), threads, [&](size_t, size_t part) {
        size_t n = 0;
        for (auto &group : sources)
            n += group[part].size();
        out[part].reserve(n);
        for (auto &group : sources) {
            auto &list = group[part];
            out[part].insert(out[part].end(), list.begin(), list.end());
            std::vector<Output>().swap(list);
        }
    });
}

// Estimate buffer capacity only. Sample each input part because row widths and
// file sizes can vary; all readers still process every row and grow as needed.
size_t estimate_rows(const std::vector<Text> &parts) {
    size_t rows = 0;
    for (Text file : parts) {
        Text body = after_header(file);
        Text sample = body.substr(0, 16 * 1024);
        if (!sample.empty()) {
            size_t observed = static_cast<size_t>(std::count(sample.begin(), sample.end(), '\n'));
            rows += body.size() / sample.size() * observed +
                    (body.size() % sample.size()) * observed / sample.size();
        }
    }
    return rows;
}

template <class T> void reserve_buckets(WorkerBuckets<T> &buckets, size_t rows) {
    size_t denominator = buckets.size() * buckets.front().size();
    // A skewed sample is allowed; cap speculative allocation before vectors grow.
    constexpr size_t max_initial_bytes = size_t{2} * 1024 * 1024 * 1024;
    size_t per = std::min((rows + rows / 8) / denominator, max_initial_bytes / sizeof(T) / denominator);
    for (auto &worker : buckets)
        for (auto &bucket : worker)
            bucket.reserve(per);
}

template <class Parse>
void collect_events(const std::vector<Text> &parts, int threads, std::vector<std::vector<Event>> &out,
                    Parse parse) {
    size_t partitions = out.size();
    WorkerBuckets<EventCandidate> parsed(threads, std::vector<std::vector<EventCandidate>>(partitions));
    auto estimate = estimate_rows(parts);
    reserve_buckets(parsed, estimate);
    std::vector<UserCache> caches(threads);
    auto marked = Clock::now();
    auto trace = [&](const char *stage) {
        auto now = Clock::now();
        if (std::getenv("SHOPPING_TRACE"))
            std::fprintf(stderr, "  %s %.3fs\n", stage,
                         std::chrono::duration<double>(now - marked).count());
        marked = now;
    };
    auto jobs = chunks(parts);
    parallel(jobs.size(), threads, [&](size_t w, size_t j) {
        lines(jobs[j], [&](Text line) {
            EventCandidate c;
            if (!parse(line, caches[w], c))
                return;
            size_t hash = c.id.hash();
            size_t part = hash & (partitions - 1);
            c.partition_hash = static_cast<uint32_t>(hash >> 32);
            parsed[w][part].push_back(c);
        });
    });
    trace("parse and partition");
    WorkerBuckets<Event> winners(threads, std::vector<std::vector<Event>>(partitions));
    reserve_buckets(winners, estimate);
    parallel(partitions, threads, [&](size_t w, size_t part) {
        size_t total = 0;
        for (auto &group : parsed)
            total += group[part].size();
        size_t capacity = 1;
        while (capacity < total + total / 3 + 1)
            capacity *= 2;
        std::vector<const EventCandidate *> table(capacity);
        for (auto &group : parsed)
            for (auto &row : group[part]) {
                size_t i = row.partition_hash & (capacity - 1);
                while (table[i] && table[i]->id != row.id)
                    i = (i + 1) & (capacity - 1);
                if (!table[i] || row.ingest < table[i]->ingest)
                    table[i] = &row;
            }
        for (auto *c : table)
            if (c && c->person != NONE)
                winners[w][c->person % partitions].push_back({c->person, c->ts, c->category, c->cents});
        for (auto &group : parsed)
            std::vector<EventCandidate>().swap(group[part]);
    });
    trace("deduplicate");
    merge_people(winners, threads, out);
    trace("merge by person");
}

template <class Parse>
void collect_clicks(const std::vector<Text> &parts, int threads, std::vector<std::vector<Click>> &out,
                    Parse parse) {
    const size_t partitions = out.size();
    WorkerBuckets<Click> rows(threads, std::vector<std::vector<Click>>(partitions));
    reserve_buckets(rows, estimate_rows(parts));
    std::vector<UserCache> caches(threads);
    auto jobs = chunks(parts);
    parallel(jobs.size(), threads, [&](size_t w, size_t j) {
        lines(jobs[j], [&](Text line) {
            Click click;
            if (parse(line, caches[w], click))
                rows[w][click.person % partitions].push_back(click);
        });
    });
    // Equal click IDs have identical fields by contract. Last-touch picks the
    // same tuple, and the sliding assist window reports distinct campaigns.
    // Both operations are invariant under repeated copies.
    merge_people(rows, threads, out);
}
} // namespace

Input ingest(const fs::path &data, int threads, int64_t start_ms, int64_t end_ms) {
    auto started = Clock::now();
    auto tick = [&](const char *name) {
        auto now = Clock::now();
        if (std::getenv("SHOPPING_TRACE"))
            std::fprintf(stderr, "ingest.%s %.3fs\n", name,
                         std::chrono::duration<double>(now - started).count());
        started = now;
    };
    threads = std::max(1, threads);
    Input out;
    auto files = std::make_shared<Mappings>();
    out.storage = files;
    People people(out.people);
    people.prepare(csv_parts(*files, data / "links"));
    Catalog catalog;
    catalog.load(out, *files, data);
    size_t partitions = 16;
    while (partitions < size_t(threads) * 4)
        partitions *= 2;
    out.events.resize(partitions);
    out.clicks.resize(partitions);
    tick("lookups");
    auto parse_event = [&](Text line, UserCache &users, EventCandidate &c) {
        std::array<Text, 7> f;
        if (!split_fields(line, f) || f[0].empty())
            return false;
        c = {Key(f[0]), users.times.valid_f1_ingest(f[3]), 0, NONE, catalog.unknown, 0, 0};
        if (f[1].empty() || f[5].empty())
            return true;
        bool purchase = f[4] == "purchase";
        if (!purchase && f[4] != "view" && f[4] != "cart")
            return true;
        c.ts = users.times.strict(f[2]);
        if (c.ts < start_ms || c.ts >= end_ms || c.ingest == INVALID_TIME || c.ts < c.ingest - DAY)
            return true;
        if (purchase) {
            c.cents = price(f[6]);
            if (!c.cents)
                return true;
            if (auto category = catalog.products.find(f[5]))
                c.category = *category;
        }
        c.person = users(people, f[1]);
        return true;
    };
    collect_events(csv_parts(*files, data / "events"), threads, out.events, parse_event);
    tick("events");
    auto parse_click = [&](Text line, UserCache &users, Click &c) {
        std::array<Text, 4> f;
        if (!split_fields(line, f) || f[0].empty() || f[1].empty())
            return false;
        int64_t ts = users.times.strict(f[3]);
        if (ts < start_ms - 7 * DAY || ts >= end_ms)
            return false;
        auto campaign = catalog.campaigns.find(f[2]);
        if (!campaign)
            return false;
        c = {users(people, f[1]), ts, *campaign, f[0]};
        return true;
    };
    collect_clicks(csv_parts(*files, data / "clicks"), threads, out.clicks, parse_click);
    people.finish();
    tick("clicks");
    return out;
}
