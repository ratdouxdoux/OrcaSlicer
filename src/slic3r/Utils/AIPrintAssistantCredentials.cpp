#include "AIPrintAssistantCredentials.hpp"

#include <stdexcept>
#include <openssl/sha.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/secretstore.h>

namespace Slic3r::AIPrintAssistant {
namespace {
#if wxUSE_SECRETSTORE
wxString credential_service(Provider provider, const std::string& profile)
{
    // Launchers can spell the same profile with relative paths or different
    // separators. Normalize it so a restart still addresses the same secret.
    auto directory = wxFileName::DirName(wxString::FromUTF8(profile));
    directory.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    auto path = directory.GetPath(wxPATH_GET_VOLUME, wxPATH_UNIX);
#ifdef _WIN32
    path.MakeLower();
#endif
    const auto    normalized = path.ToUTF8();
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(normalized.data()), normalized.length(), digest);
    static constexpr char HEX[]   = "0123456789abcdef";
    std::string           service = "Snorca/AIPrintAssistant/";
    service += provider_id(provider);
    service += '/';
    for (const auto byte : digest) {
        service += HEX[byte >> 4];
        service += HEX[byte & 15];
    }
    return wxString::FromUTF8(service);
}
#endif
} // namespace

std::string load_api_key(Provider provider, const std::string& profile)
{
#if wxUSE_SECRETSTORE
    wxLogNull     quiet;
    auto          store = wxSecretStore::GetDefault();
    wxString      username;
    wxSecretValue secret;
    if (store.IsOk() && store.Load(credential_service(provider, profile), username, secret))
        return secret.GetSize() ? std::string(static_cast<const char*>(secret.GetData()), secret.GetSize()) : std::string();
#endif
    throw std::runtime_error("The saved assistant API key could not be loaded. Unlock the operating system credential store or enter the "
                             "key again in Settings.");
}

void save_api_key(Provider provider, const std::string& profile, const std::string& key)
{
#if wxUSE_SECRETSTORE
    wxLogNull quiet;
    auto      store = wxSecretStore::GetDefault();
    if (store.IsOk()) {
        const auto service = credential_service(provider, profile);
        if (key.empty()) {
            // Replace first: Delete alone cannot distinguish an absent entry
            // from a locked store. Even a failed deletion must not retain a key.
            if (store.Save(service, "API key", wxSecretValue(wxString())) && store.Delete(service))
                return;
        } else if (store.Save(service, "API key", wxSecretValue(key.size(), key.data())))
            return;
    }
#endif
    throw std::runtime_error(
        "The assistant API key could not be saved. Enable or unlock the operating system credential store and try again.");
}

} // namespace Slic3r::AIPrintAssistant
