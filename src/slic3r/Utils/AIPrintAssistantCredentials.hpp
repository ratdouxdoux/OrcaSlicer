#ifndef slic3r_AIPrintAssistantCredentials_hpp_
#define slic3r_AIPrintAssistantCredentials_hpp_

#include <string>
#include "AIPrintAssistantProvider.hpp"

namespace Slic3r::AIPrintAssistant {

// OS credential storage is scoped to the application data directory and provider.
// Throws without including credentials if the OS store cannot be used.
std::string load_api_key(Provider provider, const std::string& profile);
void        save_api_key(Provider provider, const std::string& profile, const std::string& key);

} // namespace Slic3r::AIPrintAssistant
#endif
