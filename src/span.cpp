#include "silhouette/span.h"

#include <algorithm>
#include <utility>

namespace silhouette {

namespace {

template <typename Bytes>
std::optional<Bytes> CopyValidBytes(
    const std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() != Bytes{}.size()) {
        return std::nullopt;
    }

    const auto all_zero = std::all_of(
        bytes.begin(), bytes.end(), [](const std::byte value) {
            return value == std::byte{0};
        });
    if (all_zero) {
        return std::nullopt;
    }

    Bytes copied_bytes;
    std::copy(bytes.begin(), bytes.end(), copied_bytes.begin());
    return copied_bytes;
}

} // namespace

TraceId::TraceId(Bytes bytes) noexcept
    : bytes_{std::move(bytes)}
{
}

std::optional<TraceId> TraceId::FromBytes(
    const std::span<const std::byte> bytes) noexcept
{
    auto copied_bytes = CopyValidBytes<Bytes>(bytes);
    if (!copied_bytes) {
        return std::nullopt;
    }
    return TraceId{std::move(*copied_bytes)};
}

const TraceId::Bytes& TraceId::bytes() const noexcept
{
    return bytes_;
}

SpanId::SpanId(Bytes bytes) noexcept
    : bytes_{std::move(bytes)}
{
}

std::optional<SpanId> SpanId::FromBytes(
    const std::span<const std::byte> bytes) noexcept
{
    auto copied_bytes = CopyValidBytes<Bytes>(bytes);
    if (!copied_bytes) {
        return std::nullopt;
    }
    return SpanId{std::move(*copied_bytes)};
}

const SpanId::Bytes& SpanId::bytes() const noexcept
{
    return bytes_;
}

} // namespace silhouette
