#pragma once

// Evaluation-only instrumentation. Default builds do not evaluate macro
// arguments, read clocks/TLS, or link any measurement implementation.
#if defined(ZENASSET_STAGE_MEASUREMENTS) && ZENASSET_STAGE_MEASUREMENTS
#    include <array>
#    include <cassert>
#    include <chrono>
#    include <cstddef>
#    include <cstdint>
#    include <string_view>

namespace zenasset::detail::measurement
{
#    define ZENASSET_STAGE_LIST(X)                        \
        X(SourceAcquireParse, "source_acquire_parse")     \
        X(SourceConvert, "source_convert")                \
        X(SourceImages, "source_images_io_decode_wait")   \
        X(SourceMeshes, "source_mesh_convert_wait")       \
        X(ImportedNormalize, "imported_normalize")        \
        X(Cook, "cook")                                   \
        X(GeometryProcess, "geometry_process")            \
        X(TextureProcess, "texture_process")              \
        X(EnvironmentProcess, "environment_process")      \
        X(PageProcess, "page_process")                    \
        X(TextureEncodeQualify, "texture_encode_qualify") \
        X(HdrEncodeQualify, "hdr_encode_qualify")         \
        X(Write, "write")                                 \
        X(WriteValidate, "write_validate")                \
        X(Serialize, "serialize")                         \
        X(MeshEncode, "mesh_encode")                      \
        X(TransportEncode, "transport_encode")            \
        X(FileOpen, "file_open")                          \
        X(FileInspect, "file_inspect")                    \
        X(FileSeek, "file_seek")                          \
        X(FileRead, "file_read")                          \
        X(FileWrite, "file_write")                        \
        X(FileFlush, "file_flush")                        \
        X(FileClose, "file_close")                        \
        X(Publish, "publish")                             \
        X(BeforePublish, "before_publish")                \
        X(Read, "read")                                   \
        X(PackageOpen, "package_open")                    \
        X(PackageRead, "package_read")                    \
        X(TransportDecode, "transport_decode_path")       \
        X(Checksum, "resource_checksum")                  \
        X(MeshDecode, "mesh_decode")                      \
        X(Deserialize, "resource_deserialize_path")       \
        X(SceneValidate, "scene_validate")

enum class Stage : size_t
{
#    define ZENASSET_STAGE_ENUM(name, label) name,
    ZENASSET_STAGE_LIST(ZENASSET_STAGE_ENUM)
#    undef ZENASSET_STAGE_ENUM
        Count
};
inline constexpr std::array<std::string_view, size_t(Stage::Count)> StageNames{
#    define ZENASSET_STAGE_NAME(name, label) label,
    ZENASSET_STAGE_LIST(ZENASSET_STAGE_NAME)
#    undef ZENASSET_STAGE_NAME
};
#    undef ZENASSET_STAGE_LIST

enum class Counter : size_t
{
    ReadBytes,
    WrittenBytes,
    MeshEncodeAttempts,
    MeshEncodedResources,
    TransportEncodeAttempts,
    TransportEncodedResources,
    MeshDecodedResources,
    TransportDecodedResources,
    TextureCandidates,
    HdrCandidates,
    Count
};
inline constexpr std::array<std::string_view, size_t(Counter::Count)> CounterNames{"read_bytes",
                                                                                   "written_bytes",
                                                                                   "mesh_encode_attempts",
                                                                                   "mesh_encoded_resources",
                                                                                   "transport_encode_attempts",
                                                                                   "transport_encoded_resources",
                                                                                   "mesh_decoded_resources",
                                                                                   "transport_decoded_resources",
                                                                                   "texture_candidates",
                                                                                   "hdr_candidates"};

struct Interval
{
    uint64_t calls{0};
    uint64_t inclusiveNs{0};
    uint64_t exclusiveNs{0};
};
struct Statistics
{
    std::array<Interval, size_t(Stage::Count)>   stages{};
    std::array<uint64_t, size_t(Counter::Count)> counters{};
};

class Scope;
struct Context
{
    Statistics& statistics;
    Scope*      current{nullptr};
};
inline thread_local Context* ActiveContext = nullptr;

// Only the synchronous coordinator is bound. Worker elapsed time is not added
// to its wall time. Nested bindings restore their predecessor without sharing
// counters. Every scope must end before its binding and statistics are destroyed.
class Binding
{
public:
    explicit Binding(Statistics& statistics) : m_context{statistics}, m_previous(ActiveContext)
    {
        ActiveContext = &m_context;
    }
    Binding(const Binding&)            = delete;
    Binding& operator=(const Binding&) = delete;
    ~Binding()
    {
        assert(ActiveContext == &m_context && m_context.current == nullptr);
        ActiveContext = m_previous;
    }

private:
    Context  m_context;
    Context* m_previous;
};

class Scope
{
public:
    explicit Scope(Stage stage) : m_context(ActiveContext), m_stage(stage)
    {
        if (m_context != nullptr)
        {
            m_parent           = m_context->current;
            m_start            = std::chrono::steady_clock::now();
            m_context->current = this;
        }
    }
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope()
    {
        Stop();
    }
    void Stop()
    {
        if (m_context != nullptr)
        {
            assert(ActiveContext == m_context && m_context->current == this);
            const uint64_t elapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count());
            assert(elapsed >= m_childrenNs);
            Interval& interval = m_context->statistics.stages[size_t(m_stage)];
            ++interval.calls;
            interval.inclusiveNs += elapsed;
            interval.exclusiveNs += elapsed - m_childrenNs;
            if (m_parent != nullptr)
            {
                m_parent->m_childrenNs += elapsed;
            }
            m_context->current = m_parent;
            m_context          = nullptr;
        }
    }

private:
    Context*                              m_context;
    Stage                                 m_stage;
    Scope*                                m_parent{nullptr};
    std::chrono::steady_clock::time_point m_start{};
    uint64_t                              m_childrenNs{0};
};

inline void Add(Counter counter, uint64_t value)
{
    if (ActiveContext != nullptr)
    {
        ActiveContext->statistics.counters[size_t(counter)] += value;
    }
}
} // namespace zenasset::detail::measurement

#    define ZENASSET_STAGE_JOIN_INNER(a, b) a##b
#    define ZENASSET_STAGE_JOIN(a, b)       ZENASSET_STAGE_JOIN_INNER(a, b)
#    define ZENASSET_MEASURE_BEGIN(name, stage) \
        ::zenasset::detail::measurement::Scope name(::zenasset::detail::measurement::Stage::stage)
#    define ZENASSET_MEASURE_SCOPE(stage) ZENASSET_MEASURE_BEGIN(ZENASSET_STAGE_JOIN(zenassetMeasuredScope, __LINE__), stage)
#    define ZENASSET_MEASURE_END(name)    name.Stop()
#    define ZENASSET_MEASURE_ADD(counter, value) \
        ::zenasset::detail::measurement::Add(::zenasset::detail::measurement::Counter::counter, value)
#else
#    define ZENASSET_MEASURE_BEGIN(name, stage)
#    define ZENASSET_MEASURE_SCOPE(stage)
#    define ZENASSET_MEASURE_END(name)
#    define ZENASSET_MEASURE_ADD(counter, value)
#endif
