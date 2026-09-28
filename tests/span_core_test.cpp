#include "silhouette/span.h"
#include "silhouette/span_capture.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using silhouette::Span;
using silhouette::SpanCapture;
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

Span MakeSpan(const std::uint8_t marker, std::string operation_name)
{
    return Span{
        .trace_id = MakeTraceId(marker),
        .span_id = MakeSpanId(marker),
        .parent_span_id = std::nullopt,
        .service_name = std::string{"checkout"},
        .operation_name = std::move(operation_name),
        .start_time_unix_nano = static_cast<std::uint64_t>(marker),
        .end_time_unix_nano = static_cast<std::uint64_t>(marker) + 1,
    };
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

void CaptureFlattensCompleteBatches()
{
    SpanCapture capture;
    capture.AppendBatch({MakeSpan(1, "first"), MakeSpan(2, "second")});
    capture.AppendBatch({});
    capture.AppendBatch({MakeSpan(3, "third")});

    const auto snapshot = capture.Snapshot();
    Require(capture.size() == 3, "capture reported the wrong span count");
    Require(snapshot.size() == 3, "snapshot omitted captured spans");
    Require(snapshot[0].operation_name == "first", "first batch order changed");
    Require(snapshot[1].operation_name == "second", "first batch was split");
    Require(snapshot[2].operation_name == "third", "second batch order changed");
}

void ConcurrentBatchesPublishAtomically()
{
    constexpr std::size_t kBatchSize = 3;
    constexpr std::size_t kWriterCount = 8;
    constexpr std::size_t kExpectedSpanCount = kBatchSize * kWriterCount;

    SpanCapture capture;
    std::latch writers_ready{kWriterCount};
    std::latch start_writers{1};
    std::latch writers_published{kWriterCount};
    std::latch reader_observed_capture{1};
    std::atomic<std::size_t> active_writers{0};
    bool observed_partial_batch = false;
    std::size_t observations_while_writers_active = 0;

    std::thread reader{[&] {
        writers_ready.wait();
        start_writers.count_down();

        const auto observe_capture = [&] {
            const auto active_before =
                active_writers.load(std::memory_order_acquire);
            const auto snapshot_size = capture.Snapshot().size();
            const auto active_after =
                active_writers.load(std::memory_order_acquire);

            if (snapshot_size > 0 && active_before > 0 && active_after > 0) {
                ++observations_while_writers_active;
            }
            if (snapshot_size % kBatchSize != 0) {
                observed_partial_batch = true;
            }
        };

        while (!writers_published.try_wait()) {
            observe_capture();
            std::this_thread::yield();
        }
        observe_capture();

        reader_observed_capture.count_down();
    }};

    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for (std::size_t index = 0; index < kWriterCount; ++index) {
        writers.emplace_back([&, index] {
            active_writers.fetch_add(1, std::memory_order_release);
            writers_ready.count_down();
            start_writers.wait();

            const auto marker = static_cast<std::uint8_t>(index + 1);
            capture.AppendBatch({
                MakeSpan(marker, "one"),
                MakeSpan(marker, "two"),
                MakeSpan(marker, "three"),
            });

            writers_published.count_down();
            reader_observed_capture.wait();
            active_writers.fetch_sub(1, std::memory_order_release);
        });
    }

    reader.join();
    for (auto& writer : writers) {
        writer.join();
    }

    Require(observations_while_writers_active > 0,
            "reader did not observe capture while writers were active");
    Require(!observed_partial_batch,
            "snapshot observed part of a published batch");
    Require(capture.size() == kExpectedSpanCount,
            "concurrent capture lost spans");
    Require(capture.Snapshot().size() == kExpectedSpanCount,
            "concurrent snapshot lost spans");
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
    TestCase{"capture_flattens_complete_batches", CaptureFlattensCompleteBatches},
    TestCase{"concurrent_batches_publish_atomically",
             ConcurrentBatchesPublishAtomically},
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
