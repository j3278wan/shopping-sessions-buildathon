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
#include <unordered_map>

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
    void prefetch(size_t hash) const {
        if (!table.empty())
            __builtin_prefetch(&table[hash & (table.size() - 1)], 0, 1);
    }
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

size_t estimate_rows(const std::vector<Text> &parts);
std::vector<Text> chunks(const std::vector<Text> &parts);
template <class Func> void parallel(size_t count, int workers, Func func);

class People {
    struct FastSlot {
        std::atomic<uint64_t> suffix{0};
        uint32_t person = NONE;
    };
    // For one observed (byte length, first byte), the remaining ≤8 bytes are
    // an exact dictionary key. Other shapes, including an all-zero suffix,
    // take the general byte-string path. The shape is a storage decision only.
    size_t fast_length = 0, capacity = 0;
    unsigned char fast_prefix = 0;
    std::unique_ptr<FastSlot[]> slots;
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

    void choose_shape(const std::vector<Text> &parts) {
        std::array<size_t, 10 * 256> bins{};
        size_t sample = 0;
        std::array<Text, 2> sides;
        for (auto part : parts) {
            auto body = after_header(part);
            while (sample < 2048 && !body.empty()) {
                auto pos = body.find('\n');
                auto line = body.substr(0, pos);
                body.remove_prefix(pos == Text::npos ? body.size() : pos + 1);
                if (!split_fields(line, sides))
                    continue;
                for (auto side : sides) {
                    if (side.empty())
                        continue;
                    ++sample;
                    if (side.size() >= 2 && side.size() <= 9)
                        ++bins[side.size() * 256 + static_cast<unsigned char>(side.front())];
                }
            }
            if (sample >= 2048)
                break;
        }
        auto best = std::max_element(bins.begin(), bins.end());
        if (!sample || *best * 2 < sample)
            return;
        auto code = static_cast<size_t>(best - bins.begin());
        fast_length = code / 256;
        fast_prefix = static_cast<unsigned char>(code % 256);
        size_t target = std::max<size_t>(1024, estimate_rows(parts) * 2);
        capacity = 1024;
        while (capacity < target && capacity < (1u << 24))
            capacity *= 2;
        slots = std::make_unique<FastSlot[]>(capacity);
        names.resize(capacity);
        next.store(static_cast<uint32_t>(capacity), std::memory_order_relaxed);
    }

    // Claiming the slot itself assigns the numeric ID for a new singleton.
    // The original string view is consumed only after all workers join.
    uint32_t fast_slot(Text name, uint64_t key, size_t hash) {
        size_t pos = hash & (capacity - 1);
        for (size_t probe = 0; probe < 24; ++probe, pos = (pos + 1) & (capacity - 1)) {
            auto &slot = slots[pos];
            uint64_t found = slot.suffix.load(std::memory_order_relaxed);
            if (!found) {
                if (slot.suffix.compare_exchange_strong(found, key, std::memory_order_relaxed)) {
                    names[pos] = name;
                    return static_cast<uint32_t>(pos);
                }
            }
            if (found == key)
                return slot.person == NONE ? static_cast<uint32_t>(pos) : slot.person;
        }
        // This prefix of slots is permanently full. Falling back is stable
        // across workers and across build/run phases, regardless of schedule.
        return NONE;
    }
    uint32_t singleton(Text text, const Key &key, size_t hash) {
        auto &shard = added[(hash >> 16) % added.size()];
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.new_ids.get_or_insert(key, hash, [&] {
            const auto id = next.fetch_add(1, std::memory_order_relaxed);
            shard.names.emplace_back(text, id);
            return id;
        });
    }
    uint32_t link_id(Text text) {
        auto key = encode(text);
        if (key.last == 0) {
            auto id = fast_slot(text, key.first, key.hash());
            if (id != NONE)
                return id;
            key = Key(text);
        }
        return singleton(text, key, key.hash());
    }
    uint32_t root(uint32_t id) {
        while (id != parents[id]) {
            parents[id] = parents[parents[id]];
            id = parents[id];
        }
        return id;
    }

  public:
    explicit People(std::vector<Text> &output) : names(output) {}
    Key encode(Text text) const {
        if (text.size() == fast_length && fast_length &&
            static_cast<unsigned char>(text[0]) == fast_prefix) {
            uint64_t tail = 0;
            if (fast_length == 9)
                std::memcpy(&tail, text.data() + 1, 8);
            else
                std::memcpy(&tail, text.data() + 1, fast_length - 1);
            if (tail) {
                Key key;
                key.first = tail;
                return key;
            }
        }
        return Key(text);
    }
    void prepare(const std::vector<Text> &parts, int threads) {
        choose_shape(parts);
        const auto jobs = chunks(parts);
        using Edge = std::pair<uint32_t, uint32_t>;
        std::vector<std::vector<Edge>> edges(threads);
        const auto estimated = estimate_rows(parts);
        for (auto &out : edges)
            out.reserve(estimated / threads);
        parallel(jobs.size(), threads, [&](size_t worker, size_t j) {
            std::array<Text, 2> fields;
            lines(jobs[j], [&](Text line) {
                if (!split_fields(line, fields) || fields[0].empty() || fields[1].empty())
                    return;
                auto a = link_id(fields[0]);
                auto b = link_id(fields[1]);
                if (a != b)
                    edges[worker].emplace_back(a, b);
            });
        });
        // All names are stable once the workers join. Canonicalize exact
        // raw endpoint IDs with one union pass over compact integer pairs.
        finish();
        parents.resize(names.size());
        for (size_t i = 0; i < parents.size(); ++i)
            parents[i] = static_cast<uint32_t>(i);
        for (const auto &list : edges)
            for (auto [left, right] : list) {
                auto a = root(left), b = root(right);
                if (a == b)
                    continue;
                if (names[b] < names[a])
                    std::swap(a, b);
                parents[b] = a;
            }
        // Compress before the concurrent read phase. A long descending-name
        // link chain can otherwise leave thousands of pointers per lookup.
        for (size_t i = 0; i < parents.size(); ++i)
            if (parents[i] != i)
                parents[i] = root(static_cast<uint32_t>(i));
        parallel((capacity + 8191) / 8192, threads, [&](size_t, size_t block) {
            for (size_t i = block * 8192; i < std::min(capacity, (block + 1) * 8192); ++i) {
                if (!slots[i].suffix.load(std::memory_order_relaxed))
                    continue;
                slots[i].person = parents[i];
            }
        });
        for (auto &shard : added) {
            for (auto [name, id] : shard.names) {
                const auto canonical = root(id);
                linked.add(name, [canonical] { return canonical; });
            }
            shard.new_ids = Index<>();
            std::vector<std::pair<Text, uint32_t>>().swap(shard.names);
        }
        std::vector<uint32_t>().swap(parents);
    }
    void prefetch(const Key &key, size_t hash) const {
        if (key.last == 0)
            __builtin_prefetch(&slots[hash & (capacity - 1)], 0, 1);
        else
            linked.prefetch(hash);
    }
    uint32_t lookup(Text name, Key key, size_t hash) {
        if (key.last == 0) {
            auto id = fast_slot(name, key.first, hash);
            if (id != NONE)
                return id;
            key = Key(name);
            hash = key.hash();
        }
        if (auto p = linked.find(key, hash))
            return *p;
        return singleton(name, key, hash);
    }
    void finish() {
        names.resize(next.load());
        for (auto &s : added)
            for (auto [name, id] : s.names)
                names[id] = name;
    }
};

constexpr size_t user_lookup_batch_size = 32;

struct UserCache {
    shopping_time::Parser times;
    struct Request {Text name; Key key; size_t hash;};
    std::array<Request, user_lookup_batch_size> requests;
    size_t count=0;
    People* people=nullptr;

    // Queue independent requests to keep more than one miss in flight. The
    // shared dictionary itself contains both the key and the canonical ID.
    uint32_t queue(People& map, Text name) {
        auto key=map.encode(name);
        auto hash=key.hash();
        map.prefetch(key,hash);
        people=&map;
        requests[count]={name,key,hash};
        return static_cast<uint32_t>(count++);
    }
    uint32_t resolve(uint32_t index) {
        const auto& request=requests[index];
        return people->lookup(request.name,request.key,request.hash);
    }
};

template <class Row, class Parse, class Emit>
void batched_rows(Text input, UserCache &cache, Parse &parse, Emit emit) {
    // Work on subsequent rows while their immutable identity-link slots enter the CPU cache.
    std::array<Row, user_lookup_batch_size> rows;
    size_t n = 0;
    auto flush = [&] {
        for (size_t i = 0; i < n; ++i) {
            auto &row = rows[i];
            if (row.person != NONE)
                row.person = cache.resolve(row.person);
            emit(row);
        }
        n = 0;
        cache.count = 0;
    };
    lines(input, [&](Text line) {
        if (parse(line, cache, rows[n])) {
            if (++n == rows.size())
                flush();
        }
    });
    flush();
}

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

// Choose an optional compact representation from actual first fields.
int event_prefix(const std::vector<Text>& files) {
    std::array<unsigned,256> counts{};
    unsigned sample=0;
    for(Text file:files) {
        Text body=after_header(file);
        while(!body.empty() && sample<256) {
            auto stop=body.find('\n');
            auto line=body.substr(0,stop);
            body.remove_prefix(stop==Text::npos?body.size():stop+1);
            std::array<Text,7> f;
            if(split_fields(line,f) && !f[0].empty()) {
                ++sample;
                if(f[0].size()==9) ++counts[static_cast<unsigned char>(f[0][0])];
            }
        }
        if(sample==256)break;
    }
    const auto best=std::max_element(counts.begin(),counts.end());
    return *best ? static_cast<int>(best-counts.begin()) : -1;
}
struct EventId {
    uint64_t bits=0;
    static EventId encode(Text text,int prefix,uint32_t& external) {
        if(text.size()==9 && static_cast<unsigned char>(text[0])==prefix) {
            uint64_t word;
            std::memcpy(&word,text.data()+1,8);
            word^=word>>29;
            word*=UINT64_C(0x9e3779b97f4a7c15);
            external=0;
            return {word^(word>>32)};
        }
        external=1;
        return {reinterpret_cast<uintptr_t>(text.data())};
    }
    Text text() const {
        const char* p=reinterpret_cast<const char*>(static_cast<uintptr_t>(bits));
        const char* end=p;
        while(*end!=',' && *end!='"')++end;
        return {p,static_cast<size_t>(end-p)};
    }
};
struct EventCandidate {
    EventId id;
    int64_t ingest, ts;
    uint32_t person, category;
    int32_t cents;
    uint32_t external;
    bool non_inline() const { return external!=0; }
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

struct RelativeEvent {
    EventId id;
    int32_t ingest;
    uint32_t ts, person, purchase;
    bool non_inline() const { return (purchase >> 31)!=0; }
};
static_assert(sizeof(RelativeEvent) == 24);

template <class Stored, class Parse>
bool collect_event_rows(const std::vector<Text> &parts, int threads, std::vector<std::vector<Event>> &out,
                        int64_t start_ms, Parse parse) {
    constexpr bool compact=std::is_same_v<Stored,RelativeEvent>;
    const int64_t start_sec=start_ms/1000;
    struct Purchase{uint32_t category;int32_t cents;};
    std::vector<std::vector<Purchase>> purchases(threads);
    size_t partitions = out.size();
    auto estimate = estimate_rows(parts);
    size_t event_shards=partitions;
    size_t target=std::getenv("SHOPPING_SHARD_ROWS") ? std::stoul(std::getenv("SHOPPING_SHARD_ROWS")) : 40000;
    while (event_shards < 2048 && estimate/event_shards>target) event_shards*=2;
    WorkerBuckets<Stored> parsed(threads, std::vector<std::vector<Stored>>(event_shards));
    reserve_buckets(parsed, estimate);
    std::vector<UserCache> caches(threads);
    std::atomic<bool> needs_wide{false};
    auto marked = Clock::now();
    auto trace = [&](const char *stage) {
        auto now = Clock::now();
        if (std::getenv("SHOPPING_TRACE"))
            std::fprintf(stderr, "  %s %.3fs\n", stage, std::chrono::duration<double>(now - marked).count());
        marked = now;
    };
    auto jobs = chunks(parts);
    parallel(jobs.size(), threads, [&](size_t w, size_t j) {
        if constexpr (compact) if (needs_wide.load(std::memory_order_relaxed)) return;
        batched_rows<EventCandidate>(jobs[j], caches[w], parse, [&](EventCandidate &c) {
            const auto hash = c.external ? std::hash<Text>{}(c.id.text()) : c.id.bits;
            auto& bucket=parsed[w][hash & (event_shards - 1)];

            if constexpr (compact) {
                if (c.ingest==INVALID_TIME) {
                    needs_wide.store(true,std::memory_order_relaxed);
                    return;
                }
                int64_t seconds=c.ingest/1000-start_sec;
                if (seconds<std::numeric_limits<int32_t>::min() || seconds>std::numeric_limits<int32_t>::max()) {
                    needs_wide.store(true,std::memory_order_relaxed);
                    return;
                }
                const uint32_t time=c.person==NONE ? 0 : static_cast<uint32_t>(c.ts-start_ms);
                uint32_t detail=0;
                if(c.cents>0 && c.person!=NONE){
                    auto& detail_rows=purchases[w];
                    if(detail_rows.size()>=INT32_MAX){needs_wide.store(true,std::memory_order_relaxed);return;}
                    detail_rows.push_back({c.category,c.cents});
                    detail=static_cast<uint32_t>(detail_rows.size());
                }
                bucket.push_back({c.id,static_cast<int32_t>(seconds),time,c.person,detail|(c.external<<31)});
            } else {

                bucket.push_back(c);
            }
        });
    });
    if constexpr (compact) if (needs_wide.load(std::memory_order_relaxed)) return false;
    trace("parse and partition");
    WorkerBuckets<Event> winners(threads, std::vector<std::vector<Event>>(partitions));
    reserve_buckets(winners, estimate);
    parallel(event_shards, threads, [&](size_t w, size_t part) {
        size_t total = 0;
        for (auto &group : parsed)
            total += group[part].size();
        size_t capacity = 1;
        while (capacity < total + total / 3 + 1)
            capacity *= 2;
        if(!total)return;
        const unsigned shard_bits=__builtin_ctzll(event_shards);
        const unsigned address_bits=__builtin_ctzll(capacity);
        const unsigned ref_bits=64-__builtin_clzll(total);
        const uint64_t ref_mask=(uint64_t{1}<<ref_bits)-1;
        const unsigned probe_bits=std::min<unsigned>(5,shard_bits);
        const unsigned max_probe=1u<<probe_bits;
        std::vector<size_t> starts(parsed.size()+1,0);
        for(size_t src=0;src<parsed.size();++src)starts[src+1]=starts[src]+parsed[src][part].size();
        std::vector<uint64_t> table(capacity);
        std::unordered_map<Text,Stored*> external_keys;
        std::unordered_map<uint64_t,Stored*> crowded;
        auto pick=[&](Stored*& old, Stored& row){
            if(!old)old=&row;
            else if(row.ingest<old->ingest){old->person=NONE;old=&row;}
            else row.person=NONE;
        };
        for(size_t src=0;src<parsed.size();++src) {
            auto& rows=parsed[src][part];
            for(size_t n=0;n<rows.size();++n) {
                auto& row=rows[n];
                const uint64_t h=row.id.bits;
                if(!row.non_inline()) {
                    const uint64_t ref=starts[src]+n+1;
                    size_t slot=(h>>shard_bits)&(capacity-1);
                    const uint64_t upper=(h>>(address_bits+shard_bits))<<probe_bits;
                    bool done=false;
                    for(unsigned probe=0;probe<max_probe;++probe,slot=(slot+1)&(capacity-1)){
                        const uint64_t tag=(upper|probe)<<ref_bits;
                        const uint64_t word=table[slot];
                        if(!word){table[slot]=tag|ref;done=true;break;}
                        if((word&~ref_mask)==tag) {
                            uint64_t previous=(word&ref_mask)-1;
                            size_t owner=std::upper_bound(starts.begin(),starts.end(),previous)-starts.begin()-1;
                            auto& old=parsed[owner][part][previous-starts[owner]];
                            if(row.ingest<old.ingest){old.person=NONE; table[slot]=tag|ref;}
                            else row.person=NONE;
                            done=true;
                            break;
                        }
                    }
                    if(!done)pick(crowded[h],row);
                }else{
                    pick(external_keys[row.id.text()],row);
                }
            }
        }
        for(size_t src=0;src<parsed.size();++src) {
            auto& source=parsed[src];
            for(const auto& row:source[part]) if(row.person!=NONE) {
                int64_t ts=row.ts;
                uint32_t category=0; int32_t cents=0;
                if constexpr(compact){
                    ts+=start_ms;
                    const auto detail=row.purchase&INT32_MAX;
                    if(detail){const auto& p=purchases[src][detail-1];category=p.category;cents=p.cents;}
                } else { category=row.category;cents=row.cents; }
                winners[w][row.person & (partitions-1)].push_back({row.person,ts,category,cents});
            }
            std::vector<Stored>().swap(source[part]);
        }
    });
    trace("deduplicate");
    merge_people(winners, threads, out);
    trace("merge by person");
    return true;
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
        batched_rows<Click>(jobs[j], caches[w], parse,
                            [&](Click &click) { rows[w][click.person % partitions].push_back(click); });
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
    people.prepare(csv_parts(*files, data / "links"), threads);
    Catalog catalog;
    catalog.load(out, *files, data);
    size_t partitions = 16;
    while (partitions < size_t(threads) * 4)
        partitions *= 2;
    out.events.resize(partitions);
    out.clicks.resize(partitions);
    tick("lookups");
    auto event_files=csv_parts(*files,data/"events");
    const int selected_prefix=event_prefix(event_files);
    auto parse_event = [&](Text line, UserCache &users, EventCandidate &c) {
        std::array<Text, 7> f;
        if (!split_fields(line, f) || f[0].empty())
            return false;
        uint32_t external;
        const auto event_id=EventId::encode(f[0],selected_prefix,external);
        c = {event_id, 0, 0, NONE, catalog.unknown, 0, external};
        c.ts = users.times.event_pair(f[2], f[3], c.ingest);
        if (f[1].empty() || f[5].empty())
            return true;
        bool purchase = f[4] == "purchase";
        if (!purchase && f[4] != "view" && f[4] != "cart")
            return true;
        if (c.ts < start_ms || c.ts >= end_ms || c.ingest == INVALID_TIME || c.ts < c.ingest - DAY)
            return true;
        if (purchase) {
            c.cents = price(f[6]);
            if (!c.cents)
                return true;
            if (auto category = catalog.products.find(f[5]))
                c.category = *category;
        }
        c.person = users.queue(people, f[1]);
        return true;
    };
    const uint64_t span=uint64_t(end_ms)-uint64_t(start_ms);
    if(span>std::numeric_limits<uint32_t>::max() ||
       !collect_event_rows<RelativeEvent>(event_files,threads,out.events,start_ms,parse_event))
        collect_event_rows<EventCandidate>(event_files,threads,out.events,start_ms,parse_event);
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
        c = {users.queue(people, f[1]), ts, *campaign, f[0]};
        return true;
    };
    collect_clicks(csv_parts(*files, data / "clicks"), threads, out.clicks, parse_click);
    people.finish();
    tick("clicks");
    return out;
}
