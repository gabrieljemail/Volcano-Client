#pragma once
#ifndef VOLCANO_NETWORK_MICROSOFT_ACCOUNT_H
#define VOLCANO_NETWORK_MICROSOFT_ACCOUNT_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Volcano {

// The claims of a Minecraft services access token (a JWT), plus the raw token itself.
struct AccessToken
{
    std::string raw;
    std::string aud;
    std::string iss;
    uint8_t ver = 0;
    std::string azp;
    std::vector<std::string> scp;
    std::vector<std::string> roles;
    std::string oid;
    std::string tid;
    uint64_t iat = 0;
    uint64_t exp = 0; // Seconds since the epoch; 0 when unknown.
};

class MicrosoftAccount
{
public:
    const AccessToken& GetAccessToken() const { return accessToken; }
    void SetAccessToken(const AccessToken& value) { accessToken = value; }

    const std::string& GetRefreshToken() const { return refreshToken; }
    void SetRefreshToken(const std::string& value) { refreshToken = value; }

    // 32 hex digits, no dashes, as the Minecraft services report it.
    const std::string& GetUuid() const { return uuid; }
    void SetUuid(const std::string& value) { uuid = value; }

    const std::string& GetProfileName() const { return profileName; }
    void SetProfileName(const std::string& value) { profileName = value; }

    bool GetEntitled() const { return entitled; }
    void SetEntitled(bool value) { entitled = value; }

    // The active skin from the profile, if it has one.
    const std::string& GetSkinUrl() const { return skinUrl; }
    bool GetSkinSlim() const { return skinSlim; }
    void SetSkin(const std::string& url, bool slim)
    {
        skinUrl = url;
        skinSlim = slim;
    }

    // True when the access token is missing or about to lapse.
    bool NeedsRefresh(uint64_t nowSeconds) const
    {
        return accessToken.raw.empty() || (accessToken.exp != 0 && accessToken.exp < nowSeconds + 60);
    }

private:
    std::string uuid;
    std::string profileName;
    bool entitled = false;
    AccessToken accessToken;
    std::string refreshToken;
    std::string skinUrl;
    bool skinSlim = false;
};

} // namespace Volcano

#endif
