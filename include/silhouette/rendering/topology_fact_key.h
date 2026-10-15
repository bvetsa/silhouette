#pragma once

#include "silhouette/trace_provenance_index.h"

#include <optional>
#include <string_view>

namespace silhouette::rendering {
// Presentation identity, not an engine key. Lengths and optional markers make
// the encoding reversible even for embedded NULs or delimiter-like labels.
[[nodiscard]] std::string EncodeTopologyFact(const TopologyFact& fact);
[[nodiscard]] std::optional<TopologyFact> DecodeTopologyFact(std::string_view key);
}
