#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace silhouette {

class TraceId final {
public:
    static constexpr std::size_t kSize = 16;
    using Bytes = std::array<std::byte, kSize>;

    [[nodiscard]] static std::optional<TraceId> FromBytes(
        std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] const Bytes& bytes() const noexcept;

    bool operator==(const TraceId&) const = default;

private:
    explicit TraceId(Bytes bytes) noexcept;

    Bytes bytes_;
};

class SpanId final {
public:
    static constexpr std::size_t kSize = 8;
    using Bytes = std::array<std::byte, kSize>;

    [[nodiscard]] static std::optional<SpanId> FromBytes(
        std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] const Bytes& bytes() const noexcept;

    bool operator==(const SpanId&) const = default;

private:
    explicit SpanId(Bytes bytes) noexcept;

    Bytes bytes_;
};

struct Span final {
    TraceId trace_id;
    SpanId span_id;
    std::optional<SpanId> parent_span_id;
    std::optional<std::string> service_name;
    std::string operation_name;
    std::uint64_t start_time_unix_nano;
    std::uint64_t end_time_unix_nano;

    bool operator==(const Span&) const = default;
};

} // namespace silhouette
