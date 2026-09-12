#include "Logger.hpp"
#include <iostream>
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
    if (DEBUG_MESSAGES)
    {
        GUI::Chat::AddLine(message, 0xAAAAAAu); // dim gray, distinct from normal chat/system white
    }
}

} // namespace Volcano::Log
