#pragma once

#include "silhouette/span.h"

#include <cstddef>
#include <mutex>
#include <vector>

namespace silhouette {

class SpanCapture final {
public:
    SpanCapture() = default;

    SpanCapture(const SpanCapture&) = delete;
    SpanCapture& operator=(const SpanCapture&) = delete;
    SpanCapture(SpanCapture&&) = delete;
    SpanCapture& operator=(SpanCapture&&) = delete;

    // Publishes the complete non-empty batch while holding the capture lock.
    void AppendBatch(std::vector<Span> spans);

    [[nodiscard]] std::vector<Span> Snapshot() const;
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::vector<Span>> batches_;
    std::size_t span_count_{0};
};

} // namespace silhouette
