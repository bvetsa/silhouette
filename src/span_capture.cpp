#include "silhouette/span_capture.h"

#include <utility>

namespace silhouette {

void SpanCapture::AppendBatch(std::vector<Span> spans)
{
    if (spans.empty()) {
        return;
    }

    const auto appended_count = spans.size();
    std::scoped_lock lock{mutex_};
    batches_.push_back(std::move(spans));
    span_count_ += appended_count;
}

std::vector<Span> SpanCapture::Snapshot() const
{
    std::scoped_lock lock{mutex_};

    std::vector<Span> snapshot;
    snapshot.reserve(span_count_);
    for (const auto& batch : batches_) {
        snapshot.insert(snapshot.end(), batch.begin(), batch.end());
    }
    return snapshot;
}

std::size_t SpanCapture::size() const
{
    std::scoped_lock lock{mutex_};
    return span_count_;
}

} // namespace silhouette
