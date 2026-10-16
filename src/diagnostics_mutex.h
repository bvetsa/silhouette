#pragma once

#include <mutex>

namespace silhouette::detail {
// Query and rendering workers can share a non-thread-safe diagnostic stream.
inline std::mutex& DiagnosticsMutex()
{
    static std::mutex mutex;
    return mutex;
}
}
