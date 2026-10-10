#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace silhouette::rendering::detail {

// _spawnvp concatenates argv strings; preserve one argument under CRT parsing.
inline std::string QuoteWindowsProcessArgument(const std::string_view argument)
{
    std::string quoted{"\""};
    std::size_t backslashes = 0;
    for (const char character : argument) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        quoted.append(character == '"' ? 2 * backslashes + 1 : backslashes, '\\');
        quoted.push_back(character);
        backslashes = 0;
    }
    // Escape trailing backslashes before the closing quote as well.
    quoted.append(2 * backslashes, '\\');
    quoted.push_back('"');
    return quoted;
}

} // namespace silhouette::rendering::detail
