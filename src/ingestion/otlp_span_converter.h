#pragma once

#include "silhouette/span.h"

#include <cstdint>
#include <vector>

namespace opentelemetry::proto::collector::trace::v1 {
class ExportTraceServiceRequest;
}

namespace silhouette::ingestion::detail {

struct OtlpSpanConversionResult final {
    std::vector<Span> spans;
    std::int64_t rejected_span_count{0};
};

[[nodiscard]] OtlpSpanConversionResult ConvertOtlpSpans(
    const opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest&
        request);

} // namespace silhouette::ingestion::detail
