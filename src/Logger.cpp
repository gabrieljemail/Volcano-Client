#include "Logger.hpp"
#include <iostream>
#include "Config.hpp"
#include "renderer/gui/models/Chat.hpp"

namespace Volcano::Log {

void Info(const std::string& message)
{
    std::cout << message << std::endl;
}

void Error(const std::string& message)
{
    std::cerr << message << std::endl;
}

void Debug(const std::string& message)
{
    std::cout << message << std::endl;
    // Config isn't loaded yet for any Debug() call made before
    // GlobalState::LoadSettings runs — default to on, same as before Config
    // existed, rather than silently dropping those early messages.
    bool logMessages = Config::Active() == nullptr || std::get<bool>(Config::Active()->Get("Debug.LogMessages", true));
    if (logMessages)
    {
        GUI::Chat::AddLine(message, 0xAAAAAAu); // dim gray, distinct from normal chat/system white
    }
}

} // namespace Volcano::Log
