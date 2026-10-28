#include "CommandHelp.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace Volcano {

bool CommandHelp::Load(const std::string& path)
{
    descriptions.clear();
    std::ifstream file(path);
    if (!file) return false;

    nlohmann::json root = nlohmann::json::parse(file, nullptr, false);
    if (!root.is_object()) return false;
    for (auto it = root.begin(); it != root.end(); ++it)
    {
        if (it.value().is_string()) descriptions[it.key()] = it.value().get<std::string>();
    }
    return !descriptions.empty();
}

const std::string& CommandHelp::Describe(const std::vector<std::string>& words) const
{
    static const std::string none;
    for (size_t count = words.size(); count > 0; count--)
    {
        std::string path = words[0];
        for (size_t i = 1; i < count; i++) path += " " + words[i];
        auto found = descriptions.find(path);
        if (found != descriptions.end()) return found->second;
    }
    return none;
}

} // namespace Volcano
