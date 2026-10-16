#pragma once

#include "silhouette/observability_query_service.h"

#include <nlohmann/json.hpp>

namespace silhouette::presentation {
[[nodiscard]] std::string DescribeFact(const TopologyFact& fact);
[[nodiscard]] TraceRecordKey ParseRecordKey(std::string_view trace_id, std::string_view incarnation);
[[nodiscard]] std::optional<SupportCursor> ParseCursor(const nlohmann::json& value);
[[nodiscard]] nlohmann::json ToJson(const SupportPage& page);
[[nodiscard]] nlohmann::json ToJson(const TraceDetail& detail);
}
