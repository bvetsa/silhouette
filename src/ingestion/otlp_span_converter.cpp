#include "ingestion/otlp_span_converter.h"

#include <opentelemetry/proto/collector/trace/v1/trace_service.pb.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace silhouette::ingestion::detail {

namespace otlp = opentelemetry::proto::collector::trace::v1;
namespace otlp_trace = opentelemetry::proto::trace::v1;

namespace {

constexpr std::string_view kServiceNameKey = "service.name";

std::span<const std::byte> AsBytes(const std::string& value) noexcept
{
    return std::as_bytes(std::span{value.data(), value.size()});
}

std::optional<std::string> FindServiceName(
    const otlp_trace::ResourceSpans& resource_spans)
{
    if (!resource_spans.has_resource()) {
        return std::nullopt;
    }

    for (const auto& attribute : resource_spans.resource().attributes()) {
        if (attribute.key() != kServiceNameKey
            || !attribute.value().has_string_value()
            || attribute.value().string_value().empty()) {
            continue;
        }
        return attribute.value().string_value();
    }

    return std::nullopt;
}

std::optional<Span> ConvertSpan(
    const otlp_trace::Span& source,
    const std::optional<std::string>& service_name)
{
    const auto trace_id = TraceId::FromBytes(AsBytes(source.trace_id()));
    const auto span_id = SpanId::FromBytes(AsBytes(source.span_id()));
    if (!trace_id || !span_id) {
        return std::nullopt;
    }

    std::optional<SpanId> parent_span_id;
    if (!source.parent_span_id().empty()) {
        parent_span_id = SpanId::FromBytes(AsBytes(source.parent_span_id()));
        if (!parent_span_id) {
            return std::nullopt;
        }
    }

    return Span{
        .trace_id = *trace_id,
        .span_id = *span_id,
        .parent_span_id = std::move(parent_span_id),
        .service_name = service_name,
        .operation_name = source.name(),
        .start_time_unix_nano = source.start_time_unix_nano(),
        .end_time_unix_nano = source.end_time_unix_nano(),
    };
}

} // namespace

OtlpSpanConversionResult ConvertOtlpSpans(
    const otlp::ExportTraceServiceRequest& request)
{
    OtlpSpanConversionResult result;

    for (const auto& resource_spans : request.resource_spans()) {
        const auto service_name = FindServiceName(resource_spans);

        for (const auto& scope_spans : resource_spans.scope_spans()) {
            for (const auto& source_span : scope_spans.spans()) {
                auto converted_span = ConvertSpan(source_span, service_name);
                if (!converted_span) {
                    ++result.rejected_span_count;
                    continue;
                }
                result.spans.push_back(std::move(*converted_span));
            }
        }
    }

    return result;
}

} // namespace silhouette::ingestion::detail
