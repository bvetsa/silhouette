#include "ingestion/otlp_span_converter.h"

#include "silhouette/span.h"

#include <opentelemetry/proto/collector/trace/v1/trace_service.pb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace otlp = opentelemetry::proto::collector::trace::v1;
namespace otlp_trace = opentelemetry::proto::trace::v1;
namespace converter = silhouette::ingestion::detail;

void Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

std::string MakeIdBytes(const std::size_t size, const std::uint8_t marker)
{
    std::string bytes(size, '\0');
    bytes.back() = static_cast<char>(marker);
    return bytes;
}

silhouette::TraceId ExpectedTraceId(const std::uint8_t marker)
{
    const auto bytes = MakeIdBytes(silhouette::TraceId::kSize, marker);
    const auto id = silhouette::TraceId::FromBytes(
        std::as_bytes(std::span{bytes.data(), bytes.size()}));
    Require(id.has_value(), "expected trace ID was invalid");
    return *id;
}

silhouette::SpanId ExpectedSpanId(const std::uint8_t marker)
{
    const auto bytes = MakeIdBytes(silhouette::SpanId::kSize, marker);
    const auto id = silhouette::SpanId::FromBytes(
        std::as_bytes(std::span{bytes.data(), bytes.size()}));
    Require(id.has_value(), "expected span ID was invalid");
    return *id;
}

otlp_trace::Span* AddValidSpan(
    otlp_trace::ScopeSpans& scope,
    const std::uint8_t trace_marker,
    const std::uint8_t span_marker,
    std::string name)
{
    auto* span = scope.add_spans();
    span->set_trace_id(MakeIdBytes(silhouette::TraceId::kSize, trace_marker));
    span->set_span_id(MakeIdBytes(silhouette::SpanId::kSize, span_marker));
    span->set_name(std::move(name));
    return span;
}

void AddStringAttribute(
    otlp_trace::ResourceSpans& resource_spans,
    std::string key,
    std::string value)
{
    auto* attribute = resource_spans.mutable_resource()->add_attributes();
    attribute->set_key(std::move(key));
    attribute->mutable_value()->set_string_value(std::move(value));
}

void ConvertsExactFieldsAndParent()
{
    otlp::ExportTraceServiceRequest request;
    auto* resource = request.add_resource_spans();
    AddStringAttribute(*resource, "service.name", "checkout");
    auto* scope = resource->add_scope_spans();

    auto* root = AddValidSpan(*scope, 1, 2, "root");
    root->set_start_time_unix_nano(900);
    root->set_end_time_unix_nano(100);

    auto* child = AddValidSpan(*scope, 1, 3, "child");
    child->set_parent_span_id(MakeIdBytes(silhouette::SpanId::kSize, 2));
    child->set_start_time_unix_nano(101);
    child->set_end_time_unix_nano(202);

    const auto result = converter::ConvertOtlpSpans(request);

    Require(result.rejected_span_count == 0, "valid spans were rejected");
    Require(result.spans.size() == 2, "valid spans were not converted");
    Require(!result.spans[0].parent_span_id, "root acquired a parent");
    Require(result.spans[0].service_name == "checkout",
            "resource service name was not inherited");
    Require(result.spans[0].operation_name == "root", "root name changed");
    Require(result.spans[0].start_time_unix_nano == 900,
            "start timestamp changed");
    Require(result.spans[0].end_time_unix_nano == 100,
            "reversed end timestamp was not preserved");
    Require(result.spans[1].parent_span_id.has_value(),
            "child parent was omitted");
    Require(result.spans[1].parent_span_id == ExpectedSpanId(2),
            "child parent bytes changed");
    Require(result.spans[1].trace_id == ExpectedTraceId(1),
            "trace ID bytes changed");
    Require(result.spans[1].span_id == ExpectedSpanId(3),
            "span ID bytes changed");
}

void PreservesNestingOrderAndSelectsServiceName()
{
    otlp::ExportTraceServiceRequest request;
    auto* first_resource = request.add_resource_spans();
    AddStringAttribute(*first_resource, "service.name", "");
    auto* non_string = first_resource->mutable_resource()->add_attributes();
    non_string->set_key("service.name");
    non_string->mutable_value()->set_int_value(7);
    AddStringAttribute(*first_resource, "other", "ignored");
    AddStringAttribute(*first_resource, "service.name", "first-valid");
    AddStringAttribute(*first_resource, "service.name", "second-valid");

    auto* first_scope = first_resource->add_scope_spans();
    AddValidSpan(*first_scope, 1, 1, "a");
    AddValidSpan(*first_scope, 1, 2, "b");
    AddValidSpan(*first_resource->add_scope_spans(), 1, 3, "c");

    auto* second_resource = request.add_resource_spans();
    AddStringAttribute(*second_resource, "service.name", "");
    auto* second_non_string =
        second_resource->mutable_resource()->add_attributes();
    second_non_string->set_key("service.name");
    second_non_string->mutable_value()->set_bool_value(true);
    AddValidSpan(*second_resource->add_scope_spans(), 2, 4, "d");

    auto* third_resource = request.add_resource_spans();
    AddValidSpan(*third_resource->add_scope_spans(), 3, 5, "e");

    const auto result = converter::ConvertOtlpSpans(request);

    Require(result.rejected_span_count == 0, "valid nested spans were rejected");
    Require(result.spans.size() == 5, "nested spans were omitted");
    Require(result.spans[0].operation_name == "a"
                && result.spans[1].operation_name == "b"
                && result.spans[2].operation_name == "c"
                && result.spans[3].operation_name == "d"
                && result.spans[4].operation_name == "e",
            "resource/scope/span wire order changed");
    Require(result.spans[0].service_name == "first-valid"
                && result.spans[1].service_name == "first-valid"
                && result.spans[2].service_name == "first-valid",
            "first valid duplicate service name was not inherited");
    Require(!result.spans[3].service_name,
            "empty or non-string service name was retained");
    Require(!result.spans[4].service_name,
            "missing service name was not represented as absent");
}

void RejectsMalformedIdsAndKeepsValidSiblings()
{
    otlp::ExportTraceServiceRequest request;
    auto* scope = request.add_resource_spans()->add_scope_spans();

    AddValidSpan(*scope, 1, 1, "valid");

    auto* short_trace = AddValidSpan(*scope, 1, 2, "short-trace");
    short_trace->set_trace_id(MakeIdBytes(silhouette::TraceId::kSize - 1, 1));

    auto* zero_trace = AddValidSpan(*scope, 1, 3, "zero-trace");
    zero_trace->set_trace_id(std::string(silhouette::TraceId::kSize, '\0'));

    auto* short_span = AddValidSpan(*scope, 1, 4, "short-span");
    short_span->set_span_id(MakeIdBytes(silhouette::SpanId::kSize - 1, 1));

    auto* zero_span = AddValidSpan(*scope, 1, 5, "zero-span");
    zero_span->set_span_id(std::string(silhouette::SpanId::kSize, '\0'));

    auto* short_parent = AddValidSpan(*scope, 1, 6, "short-parent");
    short_parent->set_parent_span_id(
        MakeIdBytes(silhouette::SpanId::kSize - 1, 1));

    auto* zero_parent = AddValidSpan(*scope, 1, 7, "zero-parent");
    zero_parent->set_parent_span_id(
        std::string(silhouette::SpanId::kSize, '\0'));

    const auto result = converter::ConvertOtlpSpans(request);

    Require(result.rejected_span_count == 6,
            "converter reported the wrong rejected-span count");
    Require(result.spans.size() == 1, "valid sibling was not preserved alone");
    Require(result.spans[0].operation_name == "valid",
            "converter retained the wrong sibling");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"converts_exact_fields_and_parent", ConvertsExactFieldsAndParent},
    TestCase{"preserves_nesting_order_and_selects_service_name",
             PreservesNestingOrderAndSelectsServiceName},
    TestCase{"rejects_malformed_ids_and_keeps_valid_siblings",
             RejectsMalformedIdsAndKeepsValidSiblings},
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one converter test-case name.\n";
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

    std::cerr << "Unknown converter test case: " << requested_case << '\n';
    return 1;
}
