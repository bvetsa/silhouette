#include "silhouette/span.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using silhouette::Span;
using silhouette::SpanId;
using silhouette::TraceId;

static_assert(!std::is_default_constructible_v<TraceId>);
static_assert(!std::is_default_constructible_v<SpanId>);

void Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

TraceId MakeTraceId(const std::uint8_t marker)
{
    TraceId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    auto id = TraceId::FromBytes(bytes);
    Require(id.has_value(), "test trace ID was invalid");
    return *id;
}

SpanId MakeSpanId(const std::uint8_t marker)
{
    SpanId::Bytes bytes{};
    bytes.back() = static_cast<std::byte>(marker);
    auto id = SpanId::FromBytes(bytes);
    Require(id.has_value(), "test span ID was invalid");
    return *id;
}

void ValidIdsAllowIndividualZeroBytes()
{
    TraceId::Bytes trace_bytes{};
    trace_bytes[0] = std::byte{0x42};
    trace_bytes[7] = std::byte{0x00};
    trace_bytes[15] = std::byte{0x24};

    SpanId::Bytes span_bytes{};
    span_bytes[0] = std::byte{0x11};
    span_bytes[3] = std::byte{0x00};
    span_bytes[7] = std::byte{0x22};

    const auto trace_id = TraceId::FromBytes(trace_bytes);
    const auto span_id = SpanId::FromBytes(span_bytes);

    Require(trace_id.has_value(), "trace ID containing zero bytes was rejected");
    Require(span_id.has_value(), "span ID containing zero bytes was rejected");
    Require(trace_id->bytes() == trace_bytes, "trace ID bytes changed");
    Require(span_id->bytes() == span_bytes, "span ID bytes changed");
}

void InvalidIdsAreRejected()
{
    TraceId::Bytes zero_trace{};
    SpanId::Bytes zero_span{};

    TraceId::Bytes valid_trace{};
    valid_trace.back() = std::byte{1};
    SpanId::Bytes valid_span{};
    valid_span.back() = std::byte{1};

    Require(!TraceId::FromBytes(zero_trace), "all-zero trace ID was accepted");
    Require(!SpanId::FromBytes(zero_span), "all-zero span ID was accepted");
    Require(!TraceId::FromBytes(
                std::span<const std::byte>{valid_trace}.first(TraceId::kSize - 1)),
            "short trace ID was accepted");
    const std::array<std::byte, TraceId::kSize + 1> long_trace{
        std::byte{1}};
    Require(!TraceId::FromBytes(long_trace), "long trace ID was accepted");
    Require(!SpanId::FromBytes(
                std::span<const std::byte>{valid_span}.first(SpanId::kSize - 1)),
            "short span ID was accepted");
    const std::array<std::byte, SpanId::kSize + 1> long_span{
        std::byte{1}};
    Require(!SpanId::FromBytes(long_span), "long span ID was accepted");
}

void SpanPreservesDomainFields()
{
    const auto trace_id = MakeTraceId(1);
    const auto span_id = MakeSpanId(2);
    const auto parent_id = MakeSpanId(3);
    const Span span{
        .trace_id = trace_id,
        .span_id = span_id,
        .parent_span_id = parent_id,
        .service_name = std::string{"payments"},
        .operation_name = "charge",
        .start_time_unix_nano = 900,
        .end_time_unix_nano = 100,
    };

    Require(span.trace_id == trace_id, "trace ID changed");
    Require(span.span_id == span_id, "span ID changed");
    Require(span.parent_span_id == parent_id, "parent span ID changed");
    Require(span.service_name == "payments", "service name changed");
    Require(span.operation_name == "charge", "operation name changed");
    Require(span.start_time_unix_nano == 900, "start timestamp changed");
    Require(span.end_time_unix_nano == 100, "end timestamp changed");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"valid_ids_allow_individual_zero_bytes",
             ValidIdsAllowIndividualZeroBytes},
    TestCase{"invalid_ids_are_rejected", InvalidIdsAreRejected},
    TestCase{"span_preserves_domain_fields", SpanPreservesDomainFields},
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one core test-case name.\n";
        return 1;
    }

    const std::string_view requested_case{argv[1]};
    for (const auto& test_case : kTestCases) {
        if (test_case.name != requested_case) {
            continue;
        }

        try {
            test_case.run();
            std::cout << test_case.name << " passed.\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << test_case.name << " failed: " << error.what() << '\n';
            return 1;
        }
    }

    std::cerr << "Unknown core test case: " << requested_case << '\n';
    return 1;
}
