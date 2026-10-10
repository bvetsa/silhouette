#include "silhouette/rendering/service_graph_dot.h"
#include "silhouette/rendering/service_graph_renderer.h"
#include "rendering/service_graph_artifacts.h"
#include "rendering/windows_process_arguments.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using silhouette::ServiceGraph;
using silhouette::ServiceGraphGapKind;
using silhouette::rendering::FormatServiceGraphDot;
using silhouette::rendering::GraphvizServiceGraphRenderer;
using silhouette::rendering::WriteServiceGraphArtifacts;

void Require(const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

constexpr std::string_view kExpectedHeader{
    "digraph silhouette {\n"
    "  graph [rankdir=LR, bgcolor=\"white\", fontname=\"Helvetica\", "
    "label=\"Silhouette observed service graph\", labelloc=t, "
    "pad=0.3, nodesep=0.5, ranksep=0.8];\n"
    "  node [shape=box, style=\"rounded,filled\", color=\"#2563eb\", "
    "fillcolor=\"#eff6ff\", fontname=\"Helvetica\", fontsize=12];\n"
    "  edge [color=\"#334155\", style=solid, fontname=\"Helvetica\", "
    "arrowsize=0.8];\n"};

constexpr std::string_view kExpectedLegend{
    "\n  subgraph cluster_legend {\n"
    "    label=\"Legend\"; color=\"#cbd5e1\"; fontsize=12;\n"
    "    legend_service [label=\"Known observed service\"];\n"
    "    legend_target [label=\"Known observed service\"];\n"
    "    legend_service -> legend_target "
    "[label=\"Confirmed observed dependency\", fontsize=10];\n"
    "    legend_gap_0 [label=\"Missing\\nservice identity\", shape=diamond, "
    "style=filled, color=\"#b45309\", fillcolor=\"#fef3c7\", fontsize=10];\n"
    "    legend_gap_1 [label=\"Missing\\nparent\", shape=triangle, "
    "style=filled, color=\"#b91c1c\", fillcolor=\"#fee2e2\", fontsize=10];\n"
    "    legend_gap_2 [label=\"Ambiguous\\nparent\", shape=hexagon, "
    "style=filled, color=\"#7e22ce\", fillcolor=\"#f3e8ff\", fontsize=10];\n"
    "    legend_gap_3 [label=\"Cycle\\nparent\", shape=octagon, "
    "style=filled, color=\"#0f766e\", fillcolor=\"#ccfbf1\", fontsize=10];\n"
    "    legend_gap_0 -> legend_target [style=dashed, color=\"#b45309\", "
    "label=\"Diagnostic / incomplete relationship\", fontsize=10];\n"
    "  }\n"};

std::string ExpectedDot(const std::string_view body)
{
    return std::string{kExpectedHeader} + std::string{body}
        + std::string{kExpectedLegend} + "}\n";
}

std::string TopologyOnly(const std::string& dot)
{
    const auto legend = dot.find("\n  subgraph cluster_legend");
    Require(legend != std::string::npos, "embedded legend was missing");
    return dot.substr(0, legend);
}

void EmptyGraphHasExactLegend()
{
    const auto dot = FormatServiceGraphDot(ServiceGraph{});
    Require(dot == ExpectedDot(""), "empty graph or legend output changed");
    Require(TopologyOnly(dot).find("service_0") == std::string::npos,
            "empty graph invented a service");
}

void KnownTopologyAndIsolatedServices()
{
    const ServiceGraph graph{
        .services = {"A", "B", "C", "D", "isolated"},
        .edges = {{"A", "B"}, {"C", "D"}},
        .gaps = {},
    };
    Require(
        FormatServiceGraphDot(graph) == ExpectedDot(
            "  service_0 [label=\"A\"];\n"
            "  service_1 [label=\"B\"];\n"
            "  service_2 [label=\"C\"];\n"
            "  service_3 [label=\"D\"];\n"
            "  service_4 [label=\"isolated\"];\n"
            "  service_0 -> service_1;\n"
            "  service_2 -> service_3;\n"),
        "services, disconnected edges, or isolation were not preserved");
    Require(
        FormatServiceGraphDot(ServiceGraph{{"solo"}, {}, {}})
            == ExpectedDot("  service_0 [label=\"solo\"];\n"),
        "one isolated service did not render exactly once");
}

void ServiceLabelsAreEscaped()
{
    const ServiceGraph graph{
        .services = {"payments \"v2\" \\N\\G\\n\nline\r\ttab; } $(touch injected)"},
        .edges = {},
        .gaps = {},
    };
    Require(
        FormatServiceGraphDot(graph) == ExpectedDot(
            "  service_0 [label=\"payments \\\"v2\\\" \\\\N\\\\G\\\\n\\n"
            "line\\\\r\\\\ttab; } $(touch injected)\"];\n"),
        "service name was not safely escaped as a DOT label");

    const ServiceGraph nul{{std::string{"A\0B", 3}}, {}, {}};
    Require(FormatServiceGraphDot(nul)
                == ExpectedDot("  service_0 [label=\"A\\\\x00B\"];\n"),
            "embedded NUL broke the DOT label");
}

void MissingServiceEndpointCombinations()
{
    const ServiceGraph graph{
        .services = {"A", "B"},
        .edges = {},
        .gaps = {
            {ServiceGraphGapKind::missing_service, std::nullopt, std::nullopt},
            {ServiceGraphGapKind::missing_service, std::nullopt, "B"},
            {ServiceGraphGapKind::missing_service, "A", std::nullopt},
        },
    };
    Require(
        FormatServiceGraphDot(graph) == ExpectedDot(
            "  service_0 [label=\"A\"];\n"
            "  service_1 [label=\"B\"];\n"
            "  gap_0 [label=\"Missing\\nservice identity\", shape=diamond, "
            "style=filled, color=\"#b45309\", fillcolor=\"#fef3c7\", fontsize=10];\n"
            "  gap_1 [label=\"Missing\\nservice identity\", shape=diamond, "
            "style=filled, color=\"#b45309\", fillcolor=\"#fef3c7\", fontsize=10];\n"
            "  gap_1 -> service_1 [style=dashed, color=\"#b45309\"];\n"
            "  gap_2 [label=\"Missing\\nservice identity\", shape=diamond, "
            "style=filled, color=\"#b45309\", fillcolor=\"#fef3c7\", fontsize=10];\n"
            "  service_0 -> gap_2 [style=dashed, color=\"#b45309\"];\n"),
        "missing service endpoints produced incorrect diagnostic connections");
    Require(FormatServiceGraphDot(graph).find("<unknown-service>")
                == std::string::npos,
            "renderer invented a normal unknown-service node");
}

void ParentGapsHaveDistinctShapesAndColors()
{
    const ServiceGraph graph{
        .services = {"child"},
        .edges = {},
        .gaps = {
            {ServiceGraphGapKind::missing_parent, std::nullopt, "child"},
            {ServiceGraphGapKind::ambiguous_parent, std::nullopt, "child"},
            {ServiceGraphGapKind::cycle_parent, std::nullopt, "child"},
        },
    };
    Require(
        FormatServiceGraphDot(graph) == ExpectedDot(
            "  service_0 [label=\"child\"];\n"
            "  gap_0 [label=\"Missing\\nparent\", shape=triangle, "
            "style=filled, color=\"#b91c1c\", fillcolor=\"#fee2e2\", fontsize=10];\n"
            "  gap_0 -> service_0 [style=dashed, color=\"#b91c1c\"];\n"
            "  gap_1 [label=\"Ambiguous\\nparent\", shape=hexagon, "
            "style=filled, color=\"#7e22ce\", fillcolor=\"#f3e8ff\", fontsize=10];\n"
            "  gap_1 -> service_0 [style=dashed, color=\"#7e22ce\"];\n"
            "  gap_2 [label=\"Cycle\\nparent\", shape=octagon, "
            "style=filled, color=\"#0f766e\", fillcolor=\"#ccfbf1\", fontsize=10];\n"
            "  gap_2 -> service_0 [style=dashed, color=\"#0f766e\"];\n"),
        "parent gap appearance or relationship style changed");
}

void GapConnectionsUseOnlyStoredEndpoints()
{
    // Construct endpoint combinations intentionally: the renderer must follow
    // these fields even when a gap kind normally has unknown source upstream.
    for (const auto kind : {
             ServiceGraphGapKind::missing_service,
             ServiceGraphGapKind::missing_parent,
             ServiceGraphGapKind::ambiguous_parent,
             ServiceGraphGapKind::cycle_parent}) {
        const ServiceGraph graph{
            .services = {"A", "B"},
            .edges = {},
            .gaps = {{kind, std::nullopt, std::nullopt},
                     {kind, std::nullopt, "B"},
                     {kind, "A", std::nullopt},
                     {kind, "A", "B"}},
        };
        const auto dot = TopologyOnly(FormatServiceGraphDot(graph));
        Require(dot.find(" -> gap_0") == std::string::npos
                    && dot.find("gap_0 -> ") == std::string::npos,
                "endpoint-less fact recovered an endpoint");
        Require(dot.find("gap_1 -> service_1") != std::string::npos
                    && dot.find(" -> gap_1") == std::string::npos,
                "destination-only gap recovered a source");
        Require(dot.find("service_0 -> gap_2") != std::string::npos
                    && dot.find("gap_2 -> ") == std::string::npos,
                "source-only gap recovered a destination");
        Require(dot.find("service_0 -> gap_3") != std::string::npos
                    && dot.find("gap_3 -> service_1") != std::string::npos,
                "renderer ignored a stored endpoint because of gap kind");
        Require(dot.find("service_0 -> service_1") == std::string::npos,
                "diagnostic relationship became a confirmed edge");
    }
}

void OutputIsStableAcrossCapturePermutations()
{
    silhouette::TraceId::Bytes trace_bytes{};
    trace_bytes.back() = std::byte{1};
    const auto trace = *silhouette::TraceId::FromBytes(trace_bytes);
    const auto span_id = [](const unsigned char value) {
        silhouette::SpanId::Bytes bytes{};
        bytes.back() = static_cast<std::byte>(value);
        return *silhouette::SpanId::FromBytes(bytes);
    };
    std::vector<silhouette::Span> spans{
        {trace, span_id(1), std::nullopt, "A", "root", 1, 2},
        {trace, span_id(2), span_id(1), "B", "child", 2, 3},
        {trace, span_id(3), span_id(9), "B", "orphan", 3, 4},
    };
    const auto first = FormatServiceGraphDot(silhouette::BuildServiceGraph(
        silhouette::ReconstructTraces(spans)));
    std::reverse(spans.begin(), spans.end());
    const auto second = FormatServiceGraphDot(silhouette::BuildServiceGraph(
        silhouette::ReconstructTraces(spans)));
    Require(first == second, "DOT depended on captured span order");
}

class TemporaryWorkspace final {
public:
    TemporaryWorkspace() : original_directory_{std::filesystem::current_path()}
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("silhouette-rendering-test-" + std::to_string(stamp) + "-"
                   + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                directory_ = candidate;
                std::filesystem::current_path(directory_);
                return;
            }
        }
        throw std::runtime_error{"could not create isolated test directory"};
    }

    ~TemporaryWorkspace()
    {
        std::error_code ignored;
        std::filesystem::current_path(original_directory_, ignored);
        std::filesystem::remove_all(directory_, ignored);
    }

    TemporaryWorkspace(const TemporaryWorkspace&) = delete;
    TemporaryWorkspace& operator=(const TemporaryWorkspace&) = delete;

private:
    std::filesystem::path original_directory_;
    std::filesystem::path directory_;
};

void WriteFile(const std::filesystem::path& path, const std::string_view text)
{
    std::ofstream file{path, std::ios::binary};
    file << text;
    file.close();
    Require(!file.fail(), "could not write test fixture file");
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    Require(file.is_open(), "expected artifact was missing");
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void RealGraphvizProducesSvg()
{
    TemporaryWorkspace workspace;
    WriteFile("silhouette.svg", "stale SVG");
    const ServiceGraph graph{
        .services = {"A \"quoted\" \\N", "B"},
        .edges = {{"A \"quoted\" \\N", "B"}},
        .gaps = {
            {ServiceGraphGapKind::missing_service, std::nullopt, "B"},
            {ServiceGraphGapKind::missing_parent, std::nullopt, "B"},
            {ServiceGraphGapKind::ambiguous_parent, std::nullopt, "B"},
            {ServiceGraphGapKind::cycle_parent, std::nullopt, "B"},
        },
    };
    const auto result = WriteServiceGraphArtifacts(graph);
    Require(result.svg_rendered, result.render_error);
    Require(result.render_error.empty(), "successful render returned an error");
    Require(ReadFile("silhouette.dot") == FormatServiceGraphDot(graph),
            "written DOT differed from the pure formatter");
    const auto svg = ReadFile("silhouette.svg");
    Require(svg.find("<svg") != std::string::npos
                && svg.find("stale SVG") == std::string::npos,
            "Graphviz did not replace the stale SVG with a rendered graph");
    Require(svg.find("Legend") != std::string::npos,
            "rendered SVG omitted the embedded legend");
}

void LiveRendererReturnsOwnedSvgWithoutTouchingPublicArtifacts()
{
    TemporaryWorkspace workspace;
    WriteFile("silhouette.dot", "public DOT sentinel");
    WriteFile("silhouette.svg", "public SVG sentinel");

    GraphvizServiceGraphRenderer renderer;
    const ServiceGraph graph{
        .services = {"frontend", "checkout"},
        .edges = {{"frontend", "checkout"}},
        .gaps = {},
    };
    const auto result = renderer.RenderSvg(graph);

    Require(result.succeeded(), result.error);
    Require(result.svg.find("<svg") != std::string::npos
                && result.svg.find("frontend") != std::string::npos
                && result.svg.find("checkout") != std::string::npos,
            "live renderer did not return the complete owned SVG");
    Require(ReadFile("silhouette.dot") == "public DOT sentinel",
            "live renderer modified the public V1 DOT artifact");
    Require(ReadFile("silhouette.svg") == "public SVG sentinel",
            "live renderer modified the public V1 SVG artifact");
}

void WindowsProcessArgumentsPreservePaths()
{
    using silhouette::rendering::detail::QuoteWindowsProcessArgument;
    Require(QuoteWindowsProcessArgument(R"(C:\Users\Jane Doe\graph.dot)")
                == R"("C:\Users\Jane Doe\graph.dot")",
            "Windows input path with spaces was not quoted");
    Require(QuoteWindowsProcessArgument(R"(C:\Users\Jane Doe\graph.svg)")
                == R"("C:\Users\Jane Doe\graph.svg")",
            "Windows output path with spaces was not quoted");
    Require(QuoteWindowsProcessArgument("a\tb") == "\"a\tb\"",
            "Windows argument tab was not protected");
    Require(QuoteWindowsProcessArgument("") == "\"\"",
            "empty Windows argument was lost");
    Require(QuoteWindowsProcessArgument("a\\\"b") == "\"a\\\\\\\"b\"",
            "backslashes before a Windows quote were not escaped");
    Require(QuoteWindowsProcessArgument("C:\\path with spaces\\")
                == "\"C:\\path with spaces\\\\\"",
            "trailing Windows backslash escaped the closing quote");
}

#ifndef _WIN32
volatile std::sig_atomic_t received_graphviz_signal = 0;

extern "C" void CaptureGraphvizSignal(const int signal)
{
    received_graphviz_signal = signal;
}

class ScopedPath final {
public:
    explicit ScopedPath(const std::string& path)
    {
        if (const auto* previous = std::getenv("PATH")) {
            original_ = previous;
        }
        Require(setenv("PATH", path.c_str(), 1) == 0, "could not set test PATH");
    }
    ~ScopedPath()
    {
        if (original_) {
            setenv("PATH", original_->c_str(), 1);
        } else {
            unsetenv("PATH");
        }
    }
    ScopedPath(const ScopedPath&) = delete;
    ScopedPath& operator=(const ScopedPath&) = delete;

private:
    std::optional<std::string> original_;
};

class ScopedSignalHandler final {
public:
    ScopedSignalHandler(const int signal, void (*handler)(int))
        : signal_{signal}
        , previous_{std::signal(signal, handler)}
    {
        Require(previous_ != SIG_ERR, "could not install test signal handler");
    }

    ~ScopedSignalHandler() { std::signal(signal_, previous_); }

    ScopedSignalHandler(const ScopedSignalHandler&) = delete;
    ScopedSignalHandler& operator=(const ScopedSignalHandler&) = delete;

private:
    int signal_;
    void (*previous_)(int);
};

void LiveRendererPreservesSignalDeliveryWhileGraphvizRuns()
{
    TemporaryWorkspace workspace;
    std::filesystem::create_directory("fake-bin");
    WriteFile("fake-bin/dot",
              "#!/bin/sh\n"
              "[ \"$#\" = 4 ] && [ \"$1\" = '-Tsvg' ] "
              "&& [ \"$3\" = '-o' ] || exit 90\n"
              "kill -INT \"$PPID\"\n"
              "printf '%s' '<svg>signal-safe</svg>' > \"$4\"\n");
    std::filesystem::permissions(
        "fake-bin/dot", std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::add);
    ScopedPath path{std::filesystem::absolute("fake-bin").string()};
    received_graphviz_signal = 0;
    ScopedSignalHandler handler{SIGINT, CaptureGraphvizSignal};

    GraphvizServiceGraphRenderer renderer;
    const auto result = renderer.RenderSvg(ServiceGraph{});

    Require(result.succeeded(), result.error);
    Require(received_graphviz_signal == SIGINT,
            "live Graphviz execution swallowed the process SIGINT");
    Require(result.svg == "<svg>signal-safe</svg>",
            "controlled Graphviz process returned the wrong SVG");
}

void RequireRenderingFailure(
    const silhouette::rendering::ServiceGraphArtifactResult& result)
{
    Require(!result.svg_rendered, "failed renderer reported success");
    Require(result.render_error.find("Graphviz") != std::string::npos
                && result.render_error.find("render") != std::string::npos,
            "error did not identify Graphviz/rendering");
    Require(ReadFile("silhouette.dot") == FormatServiceGraphDot(ServiceGraph{}),
            "Graphviz failure did not retain valid DOT");
    Require(!std::filesystem::exists("silhouette.svg"),
            "failed renderer left stale or partial SVG output");
}

void UnavailableGraphvizRetainsDotAndRemovesStaleSvg()
{
    TemporaryWorkspace workspace;
    std::filesystem::create_directory("empty-path");
    ScopedPath path{std::filesystem::absolute("empty-path").string()};
    WriteFile("silhouette.svg", "stale SVG");
    RequireRenderingFailure(WriteServiceGraphArtifacts(ServiceGraph{}));
}

void FailingGraphvizRemovesPartialSvg()
{
    TemporaryWorkspace workspace;
    std::filesystem::create_directory("fake-bin");
    WriteFile("fake-bin/dot",
              "#!/bin/sh\n"
              "[ \"$#\" = 4 ] && [ \"$1\" = '-Tsvg' ] "
              "&& [ \"$2\" = 'silhouette.dot' ] "
              "&& [ \"$3\" = '-o' ] && [ \"$4\" = 'silhouette.svg' ] "
              "|| exit 90\n"
              "[ ! -e silhouette.svg ] || exit 91\n"
              "printf '%s' partial-svg > \"$4\"\n"
              "printf '%s' invoked > fake-was-invoked\n"
              "exit 23\n");
    std::filesystem::permissions(
        "fake-bin/dot", std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::add);
    ScopedPath path{std::filesystem::absolute("fake-bin").string()};
    WriteFile("silhouette.svg", "stale SVG");
    RequireRenderingFailure(WriteServiceGraphArtifacts(ServiceGraph{}));
    Require(ReadFile("fake-was-invoked") == "invoked",
            "controlled failing renderer was not exercised");
}
#endif

void DotWriteFailureHasFileContext()
{
    TemporaryWorkspace workspace;
    std::filesystem::create_directory("silhouette.dot");
    try {
        static_cast<void>(WriteServiceGraphArtifacts(ServiceGraph{}));
    } catch (const std::runtime_error& error) {
        Require(std::string_view{error.what()}.find("silhouette.dot")
                    != std::string_view::npos,
                "DOT write error omitted its file context");
        return;
    }
    throw std::runtime_error{"DOT write failure was not reported"};
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array kTestCases{
    TestCase{"windows_process_arguments_preserve_paths",
             WindowsProcessArgumentsPreservePaths},
    TestCase{"empty_graph_has_exact_legend", EmptyGraphHasExactLegend},
    TestCase{"known_topology_and_isolated_services", KnownTopologyAndIsolatedServices},
    TestCase{"service_labels_are_escaped", ServiceLabelsAreEscaped},
    TestCase{"missing_service_endpoint_combinations", MissingServiceEndpointCombinations},
    TestCase{"parent_gaps_have_distinct_shapes_and_colors", ParentGapsHaveDistinctShapesAndColors},
    TestCase{"gap_connections_use_only_stored_endpoints", GapConnectionsUseOnlyStoredEndpoints},
    TestCase{"output_is_stable_across_capture_permutations", OutputIsStableAcrossCapturePermutations},
    TestCase{"real_graphviz_produces_svg", RealGraphvizProducesSvg},
    TestCase{"live_renderer_returns_owned_svg_without_touching_public_artifacts",
             LiveRendererReturnsOwnedSvgWithoutTouchingPublicArtifacts},
    TestCase{"dot_write_failure_has_file_context", DotWriteFailureHasFileContext},
#ifndef _WIN32
    TestCase{"unavailable_graphviz_retains_dot_and_removes_stale_svg",
             UnavailableGraphvizRetainsDotAndRemovesStaleSvg},
    TestCase{"failing_graphviz_removes_partial_svg", FailingGraphvizRemovesPartialSvg},
    TestCase{"live_renderer_preserves_signal_delivery_while_graphviz_runs",
             LiveRendererPreservesSignalDeliveryWhileGraphvizRuns},
#endif
};

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        std::cerr << "Expected exactly one rendering test-case name.\n";
        return 1;
    }
    const std::string_view requested{argv[1]};
    for (const auto& test : kTestCases) {
        if (test.name == requested) {
            try {
                test.run();
                std::cout << test.name << " passed.\n";
                return 0;
            } catch (const std::exception& error) {
                std::cerr << test.name << " failed: " << error.what() << '\n';
                return 1;
            }
        }
    }
    std::cerr << "Unknown rendering test case: " << requested << '\n';
    return 1;
}
