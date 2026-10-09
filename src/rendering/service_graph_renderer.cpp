#include "silhouette/rendering/service_graph_renderer.h"

#include "silhouette/rendering/service_graph_dot.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace silhouette::rendering {
namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        static std::atomic<std::uint64_t> sequence{0};
        const auto base = std::filesystem::temp_directory_path();
        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            const auto nonce = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
            path_ = base / ("silhouette-live-" + std::to_string(nonce) + "-"
                            + std::to_string(sequence.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        throw std::runtime_error{"could not create live rendering workspace"};
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string RunGraphviz(
    const std::filesystem::path& dot_path,
    const std::filesystem::path& svg_path)
{
    std::string program{"dot"};
    std::string format{"-Tsvg"};
    std::string dot{dot_path.string()};
    std::string output_flag{"-o"};
    std::string svg{svg_path.string()};

#ifdef _WIN32
    const char* arguments[]{
        program.c_str(),
        format.c_str(),
        dot.c_str(),
        output_flag.c_str(),
        svg.c_str(),
        nullptr,
    };
    const auto status = _spawnvp(_P_WAIT, program.c_str(), arguments);
    if (status == -1) {
        return "could not launch Graphviz `dot`: "
            + std::string{std::strerror(errno)};
    }
    if (status != 0) {
        return "Graphviz `dot` failed during live rendering (exit status "
            + std::to_string(status) + ")";
    }
#else
    char* arguments[]{
        program.data(),
        format.data(),
        dot.data(),
        output_flag.data(),
        svg.data(),
        nullptr,
    };
    pid_t child = 0;
    const auto spawn_error = posix_spawnp(
        &child, program.c_str(), nullptr, nullptr, arguments, environ);
    if (spawn_error != 0) {
        return "could not launch Graphviz `dot`: "
            + std::string{std::strerror(spawn_error)};
    }

    int status = 0;
    pid_t waited = 0;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1) {
        return "could not wait for Graphviz `dot`: "
            + std::string{std::strerror(errno)};
    }
    if (WIFSIGNALED(status)) {
        return "Graphviz `dot` was terminated by signal "
            + std::to_string(WTERMSIG(status));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return "Graphviz `dot` failed during live rendering (exit status "
            + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : status)
            + ")";
    }
#endif

    return {};
}

} // namespace

SvgRenderResult GraphvizServiceGraphRenderer::RenderSvg(
    const ServiceGraph& graph)
{
    try {
        TemporaryDirectory workspace;
        const auto dot_path = workspace.path() / "graph.dot";
        const auto svg_path = workspace.path() / "graph.svg";

        {
            std::ofstream dot_file;
            dot_file.exceptions(std::ios::failbit | std::ios::badbit);
            dot_file.open(dot_path, std::ios::binary | std::ios::trunc);
            dot_file << FormatServiceGraphDot(graph);
        }

        if (auto error = RunGraphviz(dot_path, svg_path); !error.empty()) {
            return {.svg = {}, .error = std::move(error)};
        }

        std::ifstream svg_file;
        svg_file.exceptions(std::ios::failbit | std::ios::badbit);
        svg_file.open(svg_path, std::ios::binary);
        std::string svg{std::istreambuf_iterator<char>{svg_file},
                        std::istreambuf_iterator<char>{}};
        if (svg.empty()) {
            return {.svg = {},
                    .error = "Graphviz produced an empty live SVG"};
        }
        return {.svg = std::move(svg), .error = {}};
    } catch (const std::exception& error) {
        return {.svg = {},
                .error = std::string{"live SVG rendering failed: "}
                    + error.what()};
    }
}

} // namespace silhouette::rendering
