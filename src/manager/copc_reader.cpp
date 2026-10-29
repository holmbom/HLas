#include "HLas.h"
#include "byte_source.h"
#include "file_byte_source.h"
#include "geometry.h"
#include "header_parser.h"
#include "vlr_parser.h"
#include "copc_index.h"
#include "copc_query.h"
#include "dem.h"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#ifdef HLAS_HAVE_LAZ
#include "copc_node.h"
#include "laz_reader.h"
#endif

#ifdef HLAS_HAVE_NET
#include "http_byte_source.h"
#endif

namespace HLas {

Status to_status(detail::ErrorCode ec);   // definierad i reader.cpp

// Ett VLR-block större än så är inte en verklig fil utan ett försök att få oss
// att allokera. Autzens block är 1546 byte.
inline constexpr uint64_t kMaxVlrBlock = 64ull * 1024 * 1024;

struct CopcReader::Impl {
    std::unique_ptr<detail::ByteSource> source;
    detail::ParsedHeader        parsed{};
    Header                      pub_header{};
    detail::CopcInfo            info{};
    std::vector<detail::CopcEntry> nodes;
#ifdef HLAS_HAVE_LAZ
    detail::LazReader           laz;
#endif
    bool                        is_open = false;
    int                         max_connections = 4;
    // Lagras ALLTID, även i ett bygge utan nät. Structen och settern finns i
    // båda fallen — annars hade ett program inte gått att kompilera mot ett
    // bibliotek utan nätstöd, och det vore ett byggfel för något som ska vara
    // ett körningsfel. Beskedet kommer i stället från open_url().
    NetworkSettings             net_settings{};
    mutable std::string         last_error;

    // Varje punktdatafunktion börjar här, så att last_error() är tom efter ett
    // lyckat anrop och inte bär kvar ett fel från ett tidigare.
    void begin_call() const { last_error.clear(); }

    // Gemensam öppningsstig oavsett var byten kommer ifrån. Det är hela poängen
    // med ByteSource: fil och nätverk skiljer sig bara i konstruktionen.
    Status open_source(std::unique_ptr<detail::ByteSource> src);

    // Nollställer allt utom felmeddelandet. read_copc_index() fyller info och
    // nodes innan header och layout hunnit läsas, så ett fel därefter skulle
    // annars lämna node_count() och max_level() rapporterande för en läsare
    // vars open() misslyckades.
    void reset_keep_error() {
        source.reset();
        nodes.clear();
        info = detail::CopcInfo{};
        parsed = detail::ParsedHeader{};
        pub_header = Header{};
        is_open = false;
    }

    // Hämtar, packar upp och filtrerar de valda noderna.
    uint64_t collect(const detail::CopcSelection& sel,
                     const double* poly_x, const double* poly_y, uint32_t nverts,
                     double min_x, double min_y, double max_x, double max_y,
                     uint8_t* out_records,
                     uint64_t cap, const bool* accepted, int thread_count) const;

    // Strömmande varianten: samma filtrering, men punkterna lämnas vidare nod
    // för nod i stället för att samlas i anroparens buffert.
    uint64_t stream(const detail::CopcSelection& sel,
                    const double* poly_x, const double* poly_y, uint32_t nverts,
                    double min_x, double min_y, double max_x, double max_y,
                    CopcReader::PointSink sink, void* user,
                    const bool* accepted, int thread_count) const;
};

Status CopcReader::Impl::open_source(std::unique_ptr<detail::ByteSource> src) {
#ifndef HLAS_HAVE_LAZ
    (void)src;
    last_error = "LAZ decompression not supported in this build";
    return Status::LazNotSupported;
#else
    detail::ErrorCode ec = detail::read_copc_index(*src, info, nodes);
    if (ec != detail::ErrorCode::Ok) {
        last_error = detail::error_message(ec);
        reset_keep_error();
        return to_status(ec);
    }

    // Headern och VLR:erna behövs för punktlayout och skala/offset. De ligger
    // före punktdatan, så en läsning räcker.
    std::vector<uint8_t> head(parsed.offset_to_point_data ? parsed.offset_to_point_data : 4096);
    if (!src->read(0, 375, head.data())) {
        last_error = "could not read header";
        reset_keep_error();
        return Status::CorruptFile;
    }
    ec = detail::parse_header(head.data(), 375, parsed);
    if (ec != detail::ErrorCode::Ok) {
        last_error = detail::error_message(ec);
        reset_keep_error();
        return to_status(ec);
    }

    // offset_to_point_data är ett 32-bitars fält ur filen. Utan kontroll kan en
    // trasig fil be oss allokera nästan 4 GB — och en misslyckad allokering
    // kastar i stället för att ge ett Status.
    if (parsed.offset_to_point_data < parsed.header_size ||
        parsed.offset_to_point_data > src->size() ||
        parsed.offset_to_point_data > kMaxVlrBlock) {
        last_error = "implausible offset to point data";
        reset_keep_error();
        return Status::CorruptFile;
    }

    head.resize(parsed.offset_to_point_data);
    if (!src->read(0, head.size(), head.data())) {
        last_error = "could not read VLR block";
        reset_keep_error();
        return Status::CorruptFile;
    }

    std::vector<detail::Vlr> vlrs;
    ec = detail::parse_vlrs(head.data(), head.size(),
                            parsed.header_size, parsed.num_vlrs, vlrs);
    if (ec != detail::ErrorCode::Ok) {
        last_error = detail::error_message(ec);
        reset_keep_error();
        return to_status(ec);
    }

    ec = laz.init_layout(parsed, vlrs);
    if (ec != detail::ErrorCode::Ok) {
        last_error = detail::error_message(ec);
        reset_keep_error();
        return to_status(ec);
    }

    auto& h = pub_header;
    auto& p = parsed;
    h.version_major = p.version_major;  h.version_minor = p.version_minor;
    h.file_source_id = p.file_source_id; h.global_encoding = p.global_encoding;
    std::memcpy(h.system_id, p.system_id, 32);
    std::memcpy(h.generating_software, p.generating_software, 32);
    h.creation_day = p.creation_day;   h.creation_year = p.creation_year;
    h.point_format = p.point_format_id;
    h.point_record_length = p.point_record_length;
    h.point_count = p.point_count;
    std::memcpy(h.points_by_return, p.points_by_return, sizeof(h.points_by_return));
    h.x_scale = p.x_scale;   h.y_scale = p.y_scale;   h.z_scale = p.z_scale;
    h.x_offset = p.x_offset; h.y_offset = p.y_offset; h.z_offset = p.z_offset;
    h.max_x = p.max_x; h.min_x = p.min_x;
    h.max_y = p.max_y; h.min_y = p.min_y;
    h.max_z = p.max_z; h.min_z = p.min_z;
    h.is_laz = p.is_laz;
    h.num_vlrs = p.num_vlrs; h.num_evlrs = p.num_evlrs;

    source = std::move(src);
    is_open = true;
    last_error.clear();
    return Status::Ok;
#endif
}

namespace {

// Delad AABB-uträkning för polygonstigarna.
void polygon_bbox(const double* px, const double* py, uint32_t n,
                  double& min_x, double& min_y, double& max_x, double& max_y) {
    min_x = max_x = px[0];
    min_y = max_y = py[0];
    for (uint32_t v = 1; v < n; ++v) {
        if (px[v] < min_x) min_x = px[v];
        if (px[v] > max_x) max_x = px[v];
        if (py[v] < min_y) min_y = py[v];
        if (py[v] > max_y) max_y = py[v];
    }
}

// Pool av datakällor. Uppkopplingar är en annan resurs än processorkärnor:
// åtta trådar ska kunna packa upp samtidigt utan att åtta uppkopplingar öppnas
// mot någon annans server. En tråd lånar en källa för hämtningen och lämnar
// tillbaka den innan uppackningen — som är ren CPU och inte rör nätet.
class SourcePool {
public:
    SourcePool(detail::ByteSource& primary, int max_sources) {
        free_.push_back(&primary);
        for (int i = 1; i < max_sources; ++i) {
            auto c = primary.clone();
            if (!c) break;              // går inte att klona: kör med färre
            free_.push_back(c.get());
            owned_.push_back(std::move(c));
        }
    }

    int size() const { return static_cast<int>(free_.size() + borrowed_); }

    detail::ByteSource* acquire() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !free_.empty(); });
        detail::ByteSource* s = free_.back();
        free_.pop_back();
        ++borrowed_;
        return s;
    }

    void release(detail::ByteSource* s) {
        {
            std::lock_guard<std::mutex> lk(m_);
            free_.push_back(s);
            --borrowed_;
        }
        cv_.notify_one();
    }

private:
    std::mutex                m_;
    std::condition_variable   cv_;
    std::vector<detail::ByteSource*> free_;
    std::vector<std::unique_ptr<detail::ByteSource>> owned_;
    size_t                    borrowed_ = 0;
};

// Kör `work(node, pool)` över noderna på `thread_count` trådar. Poolen
// begränsar hur många hämtningar som pågår samtidigt, oberoende av trådantalet.
template <typename Work>
int run_over_nodes(const std::vector<detail::CopcEntry>& nodes,
                   SourcePool& pool, int thread_count, Work work) {
    const size_t n = nodes.size();
    if (n == 0) return 0;
    if (thread_count < 1) thread_count = 1;
    if (static_cast<size_t>(thread_count) > n)
        thread_count = static_cast<int>(n);

    if (thread_count == 1) {
        for (const auto& e : nodes)
            if (!work(e, pool)) break;
        return 1;
    }

    std::atomic<size_t> next{0};
    std::atomic<bool>   stop{false};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(thread_count));

    auto worker = [&] {
        for (;;) {
            if (stop.load(std::memory_order_relaxed)) return;
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) return;
            if (!work(nodes[i], pool)) {
                stop.store(true, std::memory_order_relaxed);
                return;
            }
        }
    };

    for (int t = 1; t < thread_count; ++t)
        threads.emplace_back(worker);
    worker();                               // huvudtråden drar också last
    for (auto& th : threads) th.join();
    return thread_count;
}

#ifdef HLAS_HAVE_LAZ
// Hämtar en nods byte genom poolen och packar upp den. Källan lånas bara under
// hämtningen; uppackningen sker utanför så att en långsam avkodning inte
// blockerar någon annans hämtning.
detail::ErrorCode fetch_and_decompress(SourcePool& pool,
                                       const detail::LazReader& laz,
                                       const detail::CopcEntry& node,
                                       std::vector<uint8_t>& chunk,
                                       std::vector<uint8_t>& out) {
    detail::ByteSource* src = pool.acquire();
    uint64_t off = 0, len = 0;
    detail::ErrorCode ec = detail::copc_node_range(node, src->size(), off, len);
    if (ec == detail::ErrorCode::Ok) {
        chunk.resize(static_cast<size_t>(len));
        if (!src->read(off, chunk.size(), chunk.data()))
            ec = detail::ErrorCode::ReadPastEnd;
    }
    pool.release(src);
    if (ec != detail::ErrorCode::Ok)
        return ec;

    return detail::decompress_copc_node(laz, node, chunk.data(), chunk.size(), out);
}
#endif // HLAS_HAVE_LAZ

} // namespace

uint64_t CopcReader::Impl::collect(const detail::CopcSelection& sel,
                                   const double* poly_x, const double* poly_y,
                                   uint32_t nverts,
                                   double min_x, double min_y,
                                   double max_x, double max_y,
                                   uint8_t* out_records,
                                   uint64_t cap, const bool* accepted,
                                   int thread_count) const {
#ifndef HLAS_HAVE_LAZ
    (void)sel; (void)poly_x; (void)poly_y; (void)nverts;
    (void)min_x; (void)min_y; (void)max_x; (void)max_y;
    (void)out_records; (void)cap; (void)accepted;
    (void)thread_count;
    return 0;
#else
    const uint16_t stride = laz.record_length();
    const uint16_t class_off = laz.classification_offset();
    const uint8_t  class_mask =
        detail::PointSource::class_mask_for_format(parsed.point_format_id);

    // Anroparen dimensionerar sin buffert som cap * stride. Ryms det talet inte
    // i size_t har DEN allokeringen redan spillt över och blivit för liten —
    // och då hade vi skrivit förbi den. Vägra i stället.
    if (stride == 0 || cap > (SIZE_MAX / stride))
        return 0;

    std::atomic<uint64_t> out{0};
    std::mutex err_mtx;

    SourcePool pool(*source, max_connections);
    run_over_nodes(sel.nodes, pool, thread_count,
        [&](const detail::CopcEntry& e, SourcePool& p) {
            if (out.load(std::memory_order_relaxed) >= cap)
                return false;

            std::vector<uint8_t> chunk, pts;
            if (fetch_and_decompress(p, laz, e, chunk, pts) != detail::ErrorCode::Ok) {
                std::lock_guard<std::mutex> lk(err_mtx);
                last_error = "could not read node";
                return false;
            }

            // Filtrera först lokalt, hävda sedan en plats i utbufferten. Att
            // skriva direkt på ett atomiskt index per punkt hade gett en
            // atomoperation per punkt i stället för en per nod.
            //
            // Posterna komprimeras ihop OFÖRÄNDRADE — filtret väljer punkter,
            // det ändrar dem inte. Det är därför RGB, intensitet och GPS-tid
            // finns kvar i det anroparen får.
            std::vector<uint8_t> keep;
            keep.reserve(static_cast<size_t>(e.point_count) * stride);
            for (int32_t i = 0; i < e.point_count; ++i) {
                const uint8_t* p = pts.data() + static_cast<size_t>(i) * stride;
                if (accepted && !accepted[p[class_off] & class_mask])
                    continue;
                int32_t ix, iy;
                std::memcpy(&ix, p,     4);
                std::memcpy(&iy, p + 4, 4);
                const double x = ix * parsed.x_scale + parsed.x_offset;
                const double y = iy * parsed.y_scale + parsed.y_offset;
                if (x < min_x || x > max_x || y < min_y || y > max_y)
                    continue;
                if (nverts >= 3 && !detail::point_in_polygon(x, y, poly_x, poly_y, nverts))
                    continue;
                keep.insert(keep.end(), p, p + stride);
            }
            const uint64_t kept = keep.size() / stride;
            if (kept == 0)
                return true;

            // Hävda en sammanhängande lucka. Blir det inte plats för hela
            // batchen tar vi det som ryms och stannar.
            uint64_t start = out.fetch_add(kept, std::memory_order_relaxed);
            if (start >= cap) { out.store(cap, std::memory_order_relaxed); return false; }
            uint64_t n = kept;
            bool full = false;
            // cap - start och inte start + n: start < cap är känt här, medan
            // summan kan spilla över.
            if (n > cap - start) { n = cap - start; full = true; }

            std::memcpy(out_records + static_cast<size_t>(start) * stride,
                        keep.data(), static_cast<size_t>(n) * stride);
            if (full) { out.store(cap, std::memory_order_relaxed); return false; }
            return true;
        });

    const uint64_t total = out.load(std::memory_order_relaxed);
    return total < cap ? total : cap;
#endif
}

uint64_t CopcReader::Impl::stream(const detail::CopcSelection& sel,
                                  const double* poly_x, const double* poly_y,
                                  uint32_t nverts,
                                  double min_x, double min_y,
                                  double max_x, double max_y,
                                  CopcReader::PointSink sink, void* user,
                                  const bool* accepted, int thread_count) const {
#ifndef HLAS_HAVE_LAZ
    (void)sel; (void)poly_x; (void)poly_y; (void)nverts;
    (void)min_x; (void)min_y; (void)max_x; (void)max_y;
    (void)sink; (void)user; (void)accepted; (void)thread_count;
    return 0;
#else
    const uint16_t stride = laz.record_length();
    const uint16_t class_off = laz.classification_offset();
    const uint8_t  class_mask =
        detail::PointSource::class_mask_for_format(parsed.point_format_id);
    const uint8_t  view_format = parsed.point_format_id;
    std::atomic<uint64_t> delivered{0};
    std::atomic<bool>     aborted{false};
    std::mutex sink_mtx, err_mtx;

    SourcePool pool(*source, max_connections);
    run_over_nodes(sel.nodes, pool, thread_count,
        [&](const detail::CopcEntry& e, SourcePool& p) {
            std::vector<uint8_t> chunk, pts;
            if (fetch_and_decompress(p, laz, e, chunk, pts) != detail::ErrorCode::Ok) {
                std::lock_guard<std::mutex> lk(err_mtx);
                last_error = "could not read node";
                return false;
            }

            // Komprimera ihop de poster som klarade filtren, oförändrade.
            // Filtreringen är ett urval, inte en omskrivning — hela posten
            // följer med ut till callbacken.
            std::vector<uint8_t> keep;
            keep.reserve(static_cast<size_t>(e.point_count) * stride);

            for (int32_t i = 0; i < e.point_count; ++i) {
                const uint8_t* p = pts.data() + static_cast<size_t>(i) * stride;
                if (accepted && !accepted[p[class_off] & class_mask])
                    continue;
                int32_t ix, iy;
                std::memcpy(&ix, p,     4);
                std::memcpy(&iy, p + 4, 4);
                const double x = ix * parsed.x_scale + parsed.x_offset;
                const double y = iy * parsed.y_scale + parsed.y_offset;
                if (x < min_x || x > max_x || y < min_y || y > max_y)
                    continue;
                if (nverts >= 3 && !detail::point_in_polygon(x, y, poly_x, poly_y, nverts))
                    continue;
                keep.insert(keep.end(), p, p + stride);
            }

            const uint64_t kept = keep.size() / stride;
            if (kept == 0)
                return true;

            // Callbacken anropas aldrig samtidigt från två trådar. Ordningen
            // mellan noder är däremot obestämd när thread_count > 1.
            std::lock_guard<std::mutex> lk(sink_mtx);

            // Kontrollen måste ske EFTER låset. Flera trådar kan ha hunnit
            // filtrera klart och köa här medan en av dem fick nej — utan den
            // här raden anropas callbacken igen efter att den sagt stopp.
            if (aborted.load(std::memory_order_relaxed))
                return false;

            delivered.fetch_add(kept, std::memory_order_relaxed);
            const PointView view(keep.data(), kept, stride, view_format,
                                 parsed.x_scale, parsed.y_scale, parsed.z_scale,
                                 parsed.x_offset, parsed.y_offset, parsed.z_offset);
            if (!sink(user, view)) {
                aborted.store(true, std::memory_order_relaxed);
                return false;
            }
            return true;
        });

    return delivered.load(std::memory_order_relaxed);
#endif
}

CopcReader::CopcReader() : impl_(std::make_unique<Impl>()) {}
CopcReader::~CopcReader() = default;
CopcReader::CopcReader(CopcReader&&) noexcept = default;
CopcReader& CopcReader::operator=(CopcReader&&) noexcept = default;

Status CopcReader::open(const char* path) {
    if (!impl_) return Status::FileOpenFailed;   // flyttad-från
    close();
    if (!path || !*path) {
        impl_->last_error = "empty path";
        return Status::FileNotFound;
    }
    auto src = std::make_unique<detail::FileByteSource>();
    detail::ErrorCode ec = src->open(path);
    if (ec != detail::ErrorCode::Ok) {
        impl_->last_error = detail::error_message(ec);
        return to_status(ec);
    }
    return impl_->open_source(std::move(src));
}

Status CopcReader::open_url(const char* url,
                            const char* username, const char* password) {
    if (!impl_) return Status::FileOpenFailed;   // flyttad-från
    close();
#ifndef HLAS_HAVE_NET
    (void)url; (void)username; (void)password;
    impl_->last_error = "HTTP support not enabled in this build (HLAS_ENABLE_NET)";
    return Status::LazNotSupported;
#else
    if (!url || !*url) {
        impl_->last_error = "empty URL";
        return Status::FileNotFound;
    }
    auto src = std::make_unique<detail::HttpByteSource>();
    detail::ErrorCode ec = src->open(url, username, password, impl_->net_settings);
    if (ec != detail::ErrorCode::Ok) {
        impl_->last_error = src->last_error().empty()
                          ? detail::error_message(ec) : src->last_error();
        return to_status(ec);
    }
    return impl_->open_source(std::move(src));
#endif
}

void CopcReader::set_network_settings(const NetworkSettings& settings) {
    if (impl_) impl_->net_settings = settings;
}

const NetworkSettings& CopcReader::network_settings() const {
    // En flyttad-från läsare har ingen Impl. Ett statiskt förvalt objekt är rätt
    // svar där: det beskriver precis vad en sådan läsare skulle koppla upp med
    // om den kunde, alltså ingenting särskilt.
    static const NetworkSettings kDefaults{};
    return impl_ ? impl_->net_settings : kDefaults;
}

void CopcReader::close() {
    if (impl_) {
        impl_->source.reset();
        impl_->nodes.clear();
        impl_->info = detail::CopcInfo{};
        impl_->parsed = detail::ParsedHeader{};
        impl_->pub_header = Header{};
        impl_->is_open = false;
        impl_->last_error.clear();
        // max_connections är en inställning, inte tillstånd — den överlever close()
    }
}

bool CopcReader::is_open() const { return impl_ && impl_->is_open; }

const std::string& CopcReader::last_error() const {
    static const std::string empty;
    return impl_ ? impl_->last_error : empty;
}

const Header& CopcReader::header() const {
    static const Header empty{};
    return impl_ ? impl_->pub_header : empty;
}

void CopcReader::set_max_connections(int n) {
    if (impl_) impl_->max_connections = n < 1 ? 1 : n;
}

int CopcReader::max_connections() const {
    return impl_ ? impl_->max_connections : 0;
}

uint32_t CopcReader::node_count() const {
    return impl_ ? static_cast<uint32_t>(impl_->nodes.size()) : 0;
}

int CopcReader::max_level() const {
    if (!impl_) return -1;
    int m = -1;
    for (const auto& e : impl_->nodes)
        if (!e.is_subpage() && e.key.level > m) m = e.key.level;
    return m;
}

bool CopcReader::root_from_extent() const {
    return impl_ && impl_->info.root_from_extent;
}

CopcReader::QueryEstimate CopcReader::estimate(double min_x, double min_y,
                                               double max_x, double max_y,
                                               int max_level) const {
    QueryEstimate q{};
    if (!impl_ || !impl_->is_open) return q;
    const auto sel = detail::select_copc_nodes(impl_->info, impl_->nodes,
                                               min_x, min_y, max_x, max_y, max_level);
    q.node_count  = static_cast<uint32_t>(sel.nodes.size());
    q.point_count = sel.point_count;
    q.byte_size   = sel.byte_size;
    return q;
}

uint64_t CopcReader::read_region(double min_x, double min_y,
                                 double max_x, double max_y,
                                 uint8_t* out_records,
                                 uint64_t max_points,
                                 const bool* accepted,
                                 int max_level,
                                 int thread_count) const {
    if (!impl_) return 0;
    impl_->begin_call();
    if (!impl_->is_open || !out_records || max_points == 0) return 0;

    if (min_x > max_x) { double t = min_x; min_x = max_x; max_x = t; }
    if (min_y > max_y) { double t = min_y; min_y = max_y; max_y = t; }

    const auto sel = detail::select_copc_nodes(impl_->info, impl_->nodes,
                                               min_x, min_y, max_x, max_y, max_level);
    return impl_->collect(sel, nullptr, nullptr, 0,
                          min_x, min_y, max_x, max_y,
                          out_records, max_points, accepted,
                          thread_count);
}

CopcReader::QueryEstimate CopcReader::estimate_polygon(const double* poly_x,
                                                       const double* poly_y,
                                                       uint32_t num_vertices,
                                                       int max_level) const {
    QueryEstimate q{};
    if (!impl_ || !impl_->is_open) return q;
    const auto sel = detail::select_copc_nodes_polygon(impl_->info, impl_->nodes,
                                                       poly_x, poly_y,
                                                       num_vertices, max_level);
    q.node_count  = static_cast<uint32_t>(sel.nodes.size());
    q.point_count = sel.point_count;
    q.byte_size   = sel.byte_size;
    return q;
}

uint64_t CopcReader::read_polygon(const double* poly_x, const double* poly_y,
                                  uint32_t num_vertices,
                                  uint8_t* out_records,
                                  uint64_t max_points,
                                  const bool* accepted,
                                  int max_level,
                                  int thread_count) const {
    if (!impl_) return 0;
    impl_->begin_call();
    if (!impl_->is_open || !poly_x || !poly_y || num_vertices < 3) return 0;
    if (!out_records || max_points == 0) return 0;

    double min_x = poly_x[0], max_x = poly_x[0];
    double min_y = poly_y[0], max_y = poly_y[0];
    for (uint32_t v = 1; v < num_vertices; ++v) {
        if (poly_x[v] < min_x) min_x = poly_x[v];
        if (poly_x[v] > max_x) max_x = poly_x[v];
        if (poly_y[v] < min_y) min_y = poly_y[v];
        if (poly_y[v] > max_y) max_y = poly_y[v];
    }

    // Mot polygonen, inte mot dess AABB. För en korridor är skillnaden att
    // nästan hela filen hämtas i onödan.
    const auto sel = detail::select_copc_nodes_polygon(impl_->info, impl_->nodes,
                                                       poly_x, poly_y,
                                                       num_vertices, max_level);
    return impl_->collect(sel, poly_x, poly_y, num_vertices,
                          min_x, min_y, max_x, max_y,
                          out_records, max_points, accepted,
                          thread_count);
}

// Vyn byggs ur headern, inte ur bufferten — bufferten är bara byte. Det är
// därför den här funktionen sitter på Readern och inte är fristående: bara den
// vet vilket punktformat och vilken skala posterna kom ur.
//
// point_record_length är samma tal som läsvägen skrev med: LazReader sätter sin
// record_length_ = header.point_record_length, alltså ur samma fält. Att ta det
// härifrån kan därför inte glida isär från postindelningen i bufferten.
PointView CopcReader::view(const uint8_t* records, uint64_t count) const {
    if (!impl_ || !impl_->is_open || !records) return PointView{};
    const auto& h = impl_->pub_header;
    return PointView(records, count, h.point_record_length, h.point_format,
                     h.x_scale, h.y_scale, h.z_scale,
                     h.x_offset, h.y_offset, h.z_offset);
}

uint64_t CopcReader::read_region_streaming(double min_x, double min_y,
                                           double max_x, double max_y,
                                           PointSink sink, void* user,
                                           const bool* accepted,
                                           int max_level,
                                           int thread_count) const {
    if (!impl_) return 0;
    impl_->begin_call();
    if (!impl_->is_open || !sink) return 0;

    if (min_x > max_x) { double t = min_x; min_x = max_x; max_x = t; }
    if (min_y > max_y) { double t = min_y; min_y = max_y; max_y = t; }

    const auto sel = detail::select_copc_nodes(impl_->info, impl_->nodes,
                                               min_x, min_y, max_x, max_y, max_level);
    return impl_->stream(sel, nullptr, nullptr, 0,
                         min_x, min_y, max_x, max_y, sink, user, accepted, thread_count);
}

uint64_t CopcReader::read_polygon_streaming(const double* poly_x, const double* poly_y,
                                            uint32_t num_vertices,
                                            PointSink sink, void* user,
                                            const bool* accepted,
                                            int max_level,
                                            int thread_count) const {
    if (!impl_) return 0;
    impl_->begin_call();
    if (!impl_->is_open || !sink) return 0;
    if (!poly_x || !poly_y || num_vertices < 3) return 0;

    double min_x, min_y, max_x, max_y;
    polygon_bbox(poly_x, poly_y, num_vertices, min_x, min_y, max_x, max_y);

    const auto sel = detail::select_copc_nodes_polygon(impl_->info, impl_->nodes,
                                                       poly_x, poly_y,
                                                       num_vertices, max_level);
    return impl_->stream(sel, poly_x, poly_y, num_vertices,
                         min_x, min_y, max_x, max_y, sink, user, accepted, thread_count);
}

DemResult CopcReader::generate_dem(const double* poly_x, const double* poly_y,
                                   uint32_t num_vertices,
                                   double cell_size,
                                   DemFill fill,
                                   const bool* accepted,
                                   int max_level,
                                   int thread_count) const {
    DemResult grid{};
    if (!impl_) return grid;
    impl_->begin_call();
    if (!impl_->is_open || !poly_x || !poly_y || num_vertices < 3) return grid;
    if (cell_size <= 0.0) return grid;

#ifdef HLAS_HAVE_LAZ
    grid = detail::dem_make_grid(poly_x, poly_y, num_vertices, cell_size);
    if (grid.elevation.empty()) return grid;

    const auto sel = detail::select_copc_nodes_polygon(impl_->info, impl_->nodes,
                                                       poly_x, poly_y,
                                                       num_vertices, max_level);

    // Binnas nod för nod in i samma rutnät. Ingen nod behöver ligga kvar när
    // nästa hämtas, så ett stråk över flera kilometer går att göra DEM av utan
    // att punkterna någonsin finns samlade i minnet.
    //
    // Parallelliseringen sker över NODER, inte inuti en nod: varje tråd hämtar
    // och packar upp sin egen nod och binnar in den. Det överlappar I/O och CPU,
    // vilket är vad mätningarna pekade på — uppackningen är konstant ~742 ms
    // medan hämtningen skalar med rundturen, så vinsten finns i båda regimerna.
    // Rutnätet är delat, därför atomisk min-skrivning.
    const bool parallel = thread_count > 1;
    std::mutex err_mtx;

    SourcePool pool(*impl_->source, impl_->max_connections);
    const int used = run_over_nodes(sel.nodes, pool, thread_count,
        [&](const detail::CopcEntry& e, SourcePool& p) {
            std::vector<uint8_t> chunk, pts;
            if (fetch_and_decompress(p, impl_->laz, e, chunk, pts)
                    != detail::ErrorCode::Ok) {
                std::lock_guard<std::mutex> lk(err_mtx);
                impl_->last_error = "could not read node";
                return false;
            }
            detail::PointSource ps{};
            ps.base         = pts.data();
            ps.count        = static_cast<uint64_t>(e.point_count);
            ps.stride       = impl_->laz.record_length();
            ps.class_offset = impl_->laz.classification_offset();
            ps.class_mask   = detail::PointSource::class_mask_for_format(
                                  impl_->parsed.point_format_id);
            ps.x_scale = impl_->parsed.x_scale;  ps.x_offset = impl_->parsed.x_offset;
            ps.y_scale = impl_->parsed.y_scale;  ps.y_offset = impl_->parsed.y_offset;
            ps.z_scale = impl_->parsed.z_scale;  ps.z_offset = impl_->parsed.z_offset;

            // thread_count=1 inuti: parallelliseringen ligger på nodnivå.
            // atomic_writes när rutnätet delas mellan trådar.
            detail::dem_accumulate(grid, ps, poly_x, poly_y, num_vertices,
                                   accepted, 1, parallel);
            return true;
        });
    (void)used;

    // Fyllnadspasset är rent CPU-arbete på rutnätet och parallelliseras internt.
    detail::dem_fill(grid, fill, thread_count);
#else
    (void)fill; (void)accepted; (void)max_level; (void)thread_count;
    impl_->last_error = "LAZ decompression not supported in this build";
#endif
    return grid;
}

} // namespace HLas
