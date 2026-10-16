#include "silhouette/rendering/topology_fact_key.h"

#include <limits>
#include <stdexcept>

namespace silhouette::rendering {
namespace {
void Append(std::string& bytes, std::string_view value)
{
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error{"topology identity is too long"};
    const auto length = static_cast<std::uint32_t>(value.size());
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<char>(length >> shift));
    bytes.append(value);
}
void AppendOptional(std::string& bytes, const std::optional<std::string>& value)
{
    bytes.push_back(value ? 1 : 0);
    if (value) Append(bytes, *value);
}
class Reader final {
public:
    explicit Reader(std::string_view bytes) : bytes_{bytes} {}
    unsigned Byte()
    {
        if (bytes_.empty()) throw std::invalid_argument{"truncated fact identity"};
        const auto value = static_cast<unsigned char>(bytes_.front());
        bytes_.remove_prefix(1); return value;
    }
    std::string String()
    {
        std::uint32_t length = 0;
        for (int i = 0; i < 4; ++i) length = (length << 8) | Byte();
        if (length > bytes_.size()) throw std::invalid_argument{"truncated fact label"};
        std::string value{bytes_.substr(0, length)};
        bytes_.remove_prefix(length); return value;
    }
    std::optional<std::string> Optional()
    {
        const auto present = Byte();
        if (present > 1) throw std::invalid_argument{"invalid optional fact label"};
        return present ? std::optional{String()} : std::nullopt;
    }
    bool empty() const { return bytes_.empty(); }
private:
    std::string_view bytes_;
};
}

std::string EncodeTopologyFact(const TopologyFact& fact)
{
    std::string bytes;
    bytes.push_back(static_cast<char>(fact.index()));
    if (const auto service = std::get_if<std::string>(&fact)) Append(bytes, *service);
    else if (const auto edge = std::get_if<ServiceEdge>(&fact)) {
        Append(bytes, edge->source_service); Append(bytes, edge->destination_service);
    } else {
        const auto& gap = std::get<ServiceGraphGap>(fact);
        bytes.push_back(static_cast<char>(gap.kind));
        AppendOptional(bytes, gap.source_service); AppendOptional(bytes, gap.destination_service);
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string key{"fact_"};
    key.reserve(5 + bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        key.push_back(hex[byte >> 4]); key.push_back(hex[byte & 15]);
    }
    return key;
}

std::optional<TopologyFact> DecodeTopologyFact(std::string_view key)
{
    if (!key.starts_with("fact_") || key.size() % 2 != 1) return std::nullopt;
    key.remove_prefix(5);
    std::string bytes;
    bytes.reserve(key.size() / 2);
    const auto digit = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < key.size(); i += 2) {
        const auto high = digit(key[i]), low = digit(key[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        bytes.push_back(static_cast<char>((high << 4) | low));
    }
    try {
        Reader reader{bytes};
        std::optional<TopologyFact> fact;
        switch (reader.Byte()) {
        case 0: fact = reader.String(); break;
        case 1: {
            auto source = reader.String(); auto destination = reader.String();
            fact = ServiceEdge{std::move(source), std::move(destination)}; break;
        }
        case 2: {
            const auto kind = reader.Byte();
            if (kind > static_cast<unsigned>(ServiceGraphGapKind::cycle_parent)) return std::nullopt;
            auto source = reader.Optional(); auto destination = reader.Optional();
            fact = ServiceGraphGap{static_cast<ServiceGraphGapKind>(kind),
                                   std::move(source), std::move(destination)}; break;
        }
        default: return std::nullopt;
        }
        return reader.empty() ? fact : std::nullopt;
    } catch (const std::invalid_argument&) { return std::nullopt; }
}
}
