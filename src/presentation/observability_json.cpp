#include "presentation/observability_json.h"
#include "silhouette/rendering/topology_fact_key.h"

#include <charconv>
#include <stdexcept>

namespace silhouette::presentation {
namespace {
using Json = nlohmann::json;
std::uint64_t Number(std::string_view text)
{
    std::uint64_t number = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()
        || std::to_string(number) != text) throw std::invalid_argument{"invalid unsigned integer"};
    return number;
}
Json Key(const TraceRecordKey& key)
{ return {{"traceId", key.trace_id.ToHex()}, {"incarnation", std::to_string(key.incarnation)}}; }
Json Metadata(const TraceRecordMetadata& metadata)
{
    auto result = Key(metadata.key);
    result["lifecycle"] = metadata.lifecycle == TraceLifecycle::active ? "active" : "quiescent";
    result["publicationSequence"] = std::to_string(metadata.publication_sequence);
    result["spanCount"] = metadata.span_count;
    result["evidenceAgeMillis"] = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(metadata.evidence_age).count());
    return result;
}
Json Cursor(const SupportCursor& cursor)
{
    auto result = Key(cursor.key);
    result["instance"] = cursor.instance.value;
    result["publicationSequence"] = std::to_string(cursor.publication_sequence);
    return result;
}
std::string_view Resolution(ParentResolution resolution)
{
    switch (resolution) {
    case ParentResolution::root: return "root";
    case ParentResolution::resolved: return "resolved";
    case ParentResolution::missing: return "missing";
    case ParentResolution::ambiguous: return "ambiguous";
    case ParentResolution::cycle: return "cycle";
    }
    throw std::invalid_argument{"unrecognized parent resolution"};
}
}

std::string DescribeFact(const TopologyFact& fact)
{
    if (const auto service = std::get_if<std::string>(&fact)) return *service;
    if (const auto edge = std::get_if<ServiceEdge>(&fact))
        return edge->source_service + " → " + edge->destination_service;
    const auto& gap = std::get<ServiceGraphGap>(fact);
    std::string label;
    switch (gap.kind) {
    case ServiceGraphGapKind::missing_service: label = "Missing service identity"; break;
    case ServiceGraphGapKind::missing_parent: label = "Missing parent"; break;
    case ServiceGraphGapKind::ambiguous_parent: label = "Ambiguous parent"; break;
    case ServiceGraphGapKind::cycle_parent: label = "Cycle parent"; break;
    }
    return label + " · " + gap.source_service.value_or("Unknown source")
        + " → " + gap.destination_service.value_or("Unknown destination");
}

TraceRecordKey ParseRecordKey(std::string_view trace_id, std::string_view incarnation)
{
    if (trace_id.size() != TraceId::kSize * 2) throw std::invalid_argument{"invalid trace ID"};
    TraceId::Bytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        unsigned byte = 0;
        const auto begin = trace_id.data() + i * 2;
        const auto parsed = std::from_chars(begin, begin + 2, byte, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != begin + 2)
            throw std::invalid_argument{"invalid trace ID"};
        bytes[i] = static_cast<std::byte>(byte);
    }
    const auto id = TraceId::FromBytes(bytes);
    const auto value = Number(incarnation);
    if (!id || value == 0) throw std::invalid_argument{"invalid trace record identity"};
    return {*id, value};
}

std::optional<SupportCursor> ParseCursor(const Json& value)
{
    if (value.is_null()) return std::nullopt;
    return SupportCursor{{value.at("instance").get<std::string>()},
        Number(value.at("publicationSequence").get<std::string>()),
        ParseRecordKey(value.at("traceId").get<std::string>(),
                       value.at("incarnation").get<std::string>())};
}

Json ToJson(const SupportPage& page)
{
    auto records = Json::array();
    for (const auto& summary : page.traces) {
        auto item = Metadata(summary.metadata);
        item["serviceCount"] = summary.service_count;
        records.push_back(std::move(item));
    }
    return {{"instance", page.instance.value},
            {"observationVersion", std::to_string(page.observation_version)},
            {"traces", std::move(records)},
            {"nextCursor", page.next_cursor ? Cursor(*page.next_cursor) : Json(nullptr)}};
}

Json ToJson(const TraceDetail& detail)
{
    auto result = Metadata(detail.metadata);
    result["instance"] = detail.instance.value;
    result["observationVersion"] = std::to_string(detail.observation_version);
    result["serviceCount"] = detail.graph.services.size();
    result["topLevelIndices"] = detail.trace.top_level_indices;
    auto spans = Json::array();
    for (const auto& reconstructed : detail.trace.spans) {
        const auto& span = reconstructed.span;
        spans.push_back({{"spanId", span.span_id.ToHex()},
            {"parentSpanId", span.parent_span_id ? Json(span.parent_span_id->ToHex()) : Json(nullptr)},
            {"service", span.service_name ? Json(*span.service_name) : Json(nullptr)},
            {"operation", span.operation_name},
            {"startTimeUnixNano", std::to_string(span.start_time_unix_nano)},
            {"endTimeUnixNano", std::to_string(span.end_time_unix_nano)},
            {"parentResolution", Resolution(reconstructed.parent_resolution)},
            {"parentIndex", reconstructed.parent_index ? Json(*reconstructed.parent_index) : Json(nullptr)},
            {"childIndices", reconstructed.child_indices},
            {"duplicateSpanId", reconstructed.duplicate_span_id}});
    }
    result["spans"] = std::move(spans);
    auto facts = Json::array();
    for (const auto& service : detail.graph.services) facts.push_back(rendering::EncodeTopologyFact(service));
    for (const auto& edge : detail.graph.edges) facts.push_back(rendering::EncodeTopologyFact(edge));
    for (const auto& gap : detail.graph.gaps) facts.push_back(rendering::EncodeTopologyFact(gap));
    result["factKeys"] = std::move(facts);
    return result;
}
}
