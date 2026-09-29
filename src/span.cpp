#include "silhouette/span.h"

#include <algorithm>
#include <utility>

namespace silhouette {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

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

template <typename Bytes>
std::string BytesToHex(const Bytes& bytes)
{
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        result.push_back(kHexDigits[value >> 4]);
        result.push_back(kHexDigits[value & 0x0f]);
    }
    return result;
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

std::string TraceId::ToHex() const
{
    return BytesToHex(bytes_);
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

std::string SpanId::ToHex() const
{
    return BytesToHex(bytes_);
}

} // namespace silhouette
