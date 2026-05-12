#pragma once

#include <string>

namespace gamescope
{
	std::string_view GetHomeDir();
	std::string GetLocalUsrDir();
	std::string GetUsrDir();
	std::string_view GetConfigDir();
}
