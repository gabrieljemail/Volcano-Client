#include "MSAccountManager.hpp"
#include "../Base64.hpp"
#include "../../Config.hpp"
#include "../../Logger.hpp"
#include "../../platform/OS.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>

namespace Volcano {

namespace
{
using Json = nlohmann::json;
using Platform::OS;

constexpr const char* MSA_SCOPE = "XboxLive.signin offline_access";
constexpr const char* MSA_DEVICE_CODE_URL = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
constexpr const char* MSA_TOKEN_URL = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
constexpr const char* ACCOUNTS_FILE = "accounts.json";
constexpr const char* NO_CLIENT_ID = "Set Auth.ClientId in settings.json to your Microsoft Entra application's ID first.";

uint64_t NowSeconds()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string UrlEncode(const std::string& text)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// One request that answers with JSON; `status` is the HTTP status even for error bodies.
bool RequestJson(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
    const std::string& body, int& status, Json& result, std::string& error)
{
    std::vector<uint8_t> response;
    if (!OS::HttpRequest(method, url, headers, body, status, response, error)) return false;
    result = Json::parse(response.begin(), response.end(), nullptr, false);
    if (result.is_discarded()) result = Json::object();
    return true;
}

bool PostJson(const std::string& url, const Json& payload, int& status, Json& result, std::string& error)
{
    return RequestJson("POST", url, { "Content-Type: application/json", "Accept: application/json" },
        payload.dump(), status, result, error);
}

bool PostForm(const std::string& url, const std::string& form, int& status, Json& result, std::string& error)
{
    return RequestJson("POST", url, { "Content-Type: application/x-www-form-urlencoded", "Accept: application/json" },
        form, status, result, error);
}

bool GetJson(const std::string& url, const std::string& bearer, int& status, Json& result, std::string& error)
{
    return RequestJson("GET", url, { "Authorization: Bearer " + bearer, "Accept: application/json" }, "",
        status, result, error);
}

std::string StringOf(const Json& object, const char* key)
{
    if (!object.is_object()) return {};
    auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

void AddClaimStrings(const Json& claims, const char* key, std::vector<std::string>& list)
{
    auto it = claims.find(key);
    if (it == claims.end()) return;
    if (it->is_array())
    {
        for (const Json& entry : *it)
        {
            if (entry.is_string()) list.push_back(entry.get<std::string>());
        }
    }
    else if (it->is_string())
    {
        list.push_back(it->get<std::string>());
    }
}

AccessToken ParseAccessToken(const std::string& raw, uint64_t fallbackExpiry)
{
    AccessToken token;
    token.raw = raw;
    token.exp = fallbackExpiry;

    size_t first = raw.find('.');
    size_t second = first == std::string::npos ? first : raw.find('.', first + 1);
    if (second == std::string::npos) return token;

    std::vector<uint8_t> payload = Base64Decode(raw.substr(first + 1, second - first - 1));
    Json claims = Json::parse(payload.begin(), payload.end(), nullptr, false);
    if (!claims.is_object()) return token;

    token.aud = StringOf(claims, "aud");
    token.iss = StringOf(claims, "iss");
    token.azp = StringOf(claims, "azp");
    token.oid = StringOf(claims, "oid");
    token.tid = StringOf(claims, "tid");
    if (claims.contains("ver") && claims["ver"].is_number()) token.ver = claims["ver"].get<uint8_t>();
    if (claims.contains("iat") && claims["iat"].is_number()) token.iat = claims["iat"].get<uint64_t>();
    if (claims.contains("exp") && claims["exp"].is_number()) token.exp = claims["exp"].get<uint64_t>();
    AddClaimStrings(claims, "scp", token.scp);
    AddClaimStrings(claims, "roles", token.roles);
    return token;
}

// Hex (32 digits, dashes ignored) to the 16 raw bytes.
std::array<uint8_t, 16> UuidBytes(const std::string& text)
{
    std::array<uint8_t, 16> bytes{};
    size_t at = 0;
    int high = -1;
    for (char c : text)
    {
        int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (value < 0) continue;
        if (high < 0)
        {
            high = value;
        }
        else if (at < 16)
        {
            bytes[at++] = static_cast<uint8_t>((high << 4) | value);
            high = -1;
        }
    }
    return bytes;
}

std::string XstsErrorText(const Json& body)
{
    uint64_t code = body.contains("XErr") && body["XErr"].is_number() ? body["XErr"].get<uint64_t>() : 0;
    switch (code)
    {
        case 2148916233: return "This Microsoft account has no Xbox profile. Sign in at xbox.com once to create one.";
        case 2148916235: return "Xbox Live isn't available in this account's country.";
        case 2148916236:
        case 2148916237: return "This account needs adult verification on Xbox.";
        case 2148916238: return "This is a child account; add it to a family first.";
        default: return "Xbox sign-in was refused (" + std::to_string(code) + ").";
    }
}
} // namespace

MSAccountManager::MSAccountManager(Config* configIn) : config(configIn) {}

MSAccountManager::~MSAccountManager()
{
    if (deviceThread.joinable())
    {
        deviceThread.request_stop();
        deviceThread.join();
    }
}

std::string MSAccountManager::ClientId()
{
    return config != nullptr ? std::get<std::string>(config->Get("Auth.ClientId", std::string(""))) : std::string();
}

void MSAccountManager::Load()
{
    std::string path = config != nullptr ? config->ResolvePath(ACCOUNTS_FILE) : ACCOUNTS_FILE;
    std::ifstream file(path);
    if (!file.is_open()) return;
    Json root = Json::parse(file, nullptr, false);
    if (!root.is_object()) return;

    std::lock_guard<std::mutex> lock(accountsMutex);
    accounts.clear();
    for (const Json& entry : root.value("accounts", Json::array()))
    {
        MicrosoftAccount account;
        account.SetUuid(StringOf(entry, "uuid"));
        account.SetProfileName(StringOf(entry, "name"));
        account.SetRefreshToken(StringOf(entry, "refreshToken"));
        account.SetEntitled(entry.value("entitled", false));
        account.SetSkin(StringOf(entry, "skinUrl"), entry.value("skinSlim", false));
        if (!account.GetRefreshToken().empty()) accounts.push_back(account);
    }

    currentIndex.reset();
    if (root.contains("current") && root["current"].is_number_integer())
    {
        int64_t index = root["current"].get<int64_t>();
        if (index >= 0 && static_cast<size_t>(index) < accounts.size()) currentIndex = static_cast<size_t>(index);
    }
}

void MSAccountManager::Save()
{
    Json root;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        root["current"] = currentIndex.has_value() ? static_cast<int64_t>(*currentIndex) : int64_t{-1};
        root["accounts"] = Json::array();
        for (const MicrosoftAccount& account : accounts)
        {
            root["accounts"].push_back({ { "uuid", account.GetUuid() }, { "name", account.GetProfileName() },
                { "refreshToken", account.GetRefreshToken() }, { "entitled", account.GetEntitled() },
                { "skinUrl", account.GetSkinUrl() }, { "skinSlim", account.GetSkinSlim() } });
        }
    }

    std::string path = config != nullptr ? config->ResolvePath(ACCOUNTS_FILE) : ACCOUNTS_FILE;
    std::ofstream file(path, std::ios::trunc);
    if (file.is_open()) file << root.dump(4);
    else Log::Error("[AUTH] Couldn't write " + path);
}

size_t MSAccountManager::Count()
{
    std::lock_guard<std::mutex> lock(accountsMutex);
    return accounts.size();
}

std::optional<size_t> MSAccountManager::CurrentIndex()
{
    std::lock_guard<std::mutex> lock(accountsMutex);
    return currentIndex;
}

void MSAccountManager::SetCurrent(std::optional<size_t> index)
{
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index.has_value() && *index >= accounts.size()) return;
        currentIndex = index;
    }
    Save();
}

void MSAccountManager::Remove(size_t index)
{
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index >= accounts.size()) return;
        accounts.erase(accounts.begin() + static_cast<std::ptrdiff_t>(index));
        if (currentIndex.has_value())
        {
            if (*currentIndex == index) currentIndex.reset();
            else if (*currentIndex > index) currentIndex = *currentIndex - 1;
        }
    }
    Save();
}

std::vector<MicrosoftAccount> MSAccountManager::Snapshot()
{
    std::lock_guard<std::mutex> lock(accountsMutex);
    return accounts;
}

void MSAccountManager::SetDeviceLogin(DeviceLoginState::Phase phase, const std::string& error)
{
    std::lock_guard<std::mutex> lock(deviceLoginMutex);
    deviceLogin.phase = phase;
    deviceLogin.error = error;
}

DeviceLoginState MSAccountManager::GetDeviceLogin()
{
    std::lock_guard<std::mutex> lock(deviceLoginMutex);
    return deviceLogin;
}

void MSAccountManager::BeginDeviceLogin()
{
    if (deviceThread.joinable())
    {
        deviceThread.request_stop();
        deviceThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(deviceLoginMutex);
        deviceLogin = DeviceLoginState{};
        deviceLogin.phase = DeviceLoginState::Phase::WaitingForCode;
    }
    deviceThread = std::jthread([this](std::stop_token stopToken) { RunDeviceLogin(stopToken); });
}

void MSAccountManager::CancelDeviceLogin()
{
    if (!deviceThread.joinable()) return;
    deviceThread.request_stop();
    deviceThread.join();
    SetDeviceLogin(DeviceLoginState::Phase::Idle);
}

void MSAccountManager::RunDeviceLogin(std::stop_token stopToken)
{
    std::string clientId = ClientId();
    if (clientId.empty())
    {
        SetDeviceLogin(DeviceLoginState::Phase::Failed, NO_CLIENT_ID);
        return;
    }

    int status = 0;
    Json reply;
    std::string error;
    if (!PostForm(MSA_DEVICE_CODE_URL, "client_id=" + UrlEncode(clientId) + "&scope=" + UrlEncode(MSA_SCOPE),
            status, reply, error) || status != 200)
    {
        SetDeviceLogin(DeviceLoginState::Phase::Failed,
            error.empty() ? StringOf(reply, "error_description") : error);
        return;
    }

    std::string deviceCode = StringOf(reply, "device_code");
    int interval = reply.value("interval", 5);
    int expiresIn = reply.value("expires_in", 900);
    {
        std::lock_guard<std::mutex> lock(deviceLoginMutex);
        deviceLogin.userCode = StringOf(reply, "user_code");
        deviceLogin.verificationUri = StringOf(reply, "verification_uri");
        deviceLogin.phase = DeviceLoginState::Phase::WaitingForUser;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expiresIn);
    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline)
    {
        // Sleep in slices so a cancel doesn't wait out the whole interval.
        for (int i = 0; i < interval * 10 && !stopToken.stop_requested(); i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stopToken.stop_requested()) return;

        Json token;
        if (!PostForm(MSA_TOKEN_URL, "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id="
                + UrlEncode(clientId) + "&device_code=" + UrlEncode(deviceCode), status, token, error))
        {
            continue; // A dropped connection is worth another try.
        }

        if (status == 200)
        {
            SetDeviceLogin(DeviceLoginState::Phase::SigningIn);
            MicrosoftAccount account;
            account.SetRefreshToken(StringOf(token, "refresh_token"));
            if (!SignInWithMicrosoftToken(StringOf(token, "access_token"), account, error))
            {
                SetDeviceLogin(DeviceLoginState::Phase::Failed, error);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(accountsMutex);
                // The same profile signing in again replaces its entry.
                size_t at = accounts.size();
                for (size_t i = 0; i < accounts.size(); i++)
                {
                    if (accounts[i].GetUuid() == account.GetUuid()) at = i;
                }
                if (at == accounts.size()) accounts.push_back(account);
                else accounts[at] = account;
                currentIndex = at;
            }
            Save();
            Log::Info("[AUTH] Signed in as " + account.GetProfileName());
            SetDeviceLogin(DeviceLoginState::Phase::Done);
            return;
        }

        std::string code = StringOf(token, "error");
        if (code == "authorization_pending") continue;
        if (code == "slow_down")
        {
            interval += 5;
            continue;
        }
        SetDeviceLogin(DeviceLoginState::Phase::Failed, code == "authorization_declined" ? "Sign-in was declined."
            : code == "expired_token" ? "The code expired." : StringOf(token, "error_description"));
        return;
    }
    if (!stopToken.stop_requested()) SetDeviceLogin(DeviceLoginState::Phase::Failed, "The code expired.");
}

bool MSAccountManager::SignInWithMicrosoftToken(const std::string& msaAccessToken, MicrosoftAccount& account,
    std::string& error)
{
    int status = 0;
    Json reply;

    // Xbox Live user token.
    Json xblRequest = { { "Properties", { { "AuthMethod", "RPS" }, { "SiteName", "user.auth.xboxlive.com" },
        { "RpsTicket", "d=" + msaAccessToken } } }, { "RelyingParty", "http://auth.xboxlive.com" },
        { "TokenType", "JWT" } };
    if (!PostJson("https://user.auth.xboxlive.com/user/authenticate", xblRequest, status, reply, error)) return false;
    if (status != 200)
    {
        error = "Xbox Live sign-in failed (" + std::to_string(status) + ").";
        return false;
    }
    std::string xblToken = StringOf(reply, "Token");
    std::string userHash;
    if (reply.contains("DisplayClaims") && reply["DisplayClaims"].contains("xui") && !reply["DisplayClaims"]["xui"].empty())
    {
        userHash = StringOf(reply["DisplayClaims"]["xui"][0], "uhs");
    }

    // XSTS token for the Minecraft services.
    Json xstsRequest = { { "Properties", { { "SandboxId", "RETAIL" }, { "UserTokens", Json::array({ xblToken }) } } },
        { "RelyingParty", "rp://api.minecraftservices.com/" }, { "TokenType", "JWT" } };
    if (!PostJson("https://xsts.auth.xboxlive.com/xsts/authorize", xstsRequest, status, reply, error)) return false;
    if (status != 200)
    {
        error = XstsErrorText(reply);
        return false;
    }
    std::string xstsToken = StringOf(reply, "Token");

    // Minecraft access token.
    Json mcRequest = { { "identityToken", "XBL3.0 x=" + userHash + ";" + xstsToken } };
    if (!PostJson("https://api.minecraftservices.com/authentication/login_with_xbox", mcRequest, status, reply, error))
    {
        return false;
    }
    if (status != 200)
    {
        error = "Minecraft sign-in failed (" + std::to_string(status) + ").";
        return false;
    }
    uint64_t expiry = NowSeconds() + reply.value("expires_in", uint64_t{86400});
    account.SetAccessToken(ParseAccessToken(StringOf(reply, "access_token"), expiry));
    const std::string bearer = account.GetAccessToken().raw;

    // Ownership, then the profile (name, uuid, skin).
    Json entitlements;
    if (!GetJson("https://api.minecraftservices.com/entitlements/mcstore", bearer, status, entitlements, error))
    {
        return false;
    }
    account.SetEntitled(status == 200 && entitlements.contains("items") && !entitlements["items"].empty());

    Json profile;
    if (!GetJson("https://api.minecraftservices.com/minecraft/profile", bearer, status, profile, error)) return false;
    if (status != 200)
    {
        error = status == 404 ? "This account doesn't own Minecraft: Java Edition."
            : "Couldn't read the Minecraft profile (" + std::to_string(status) + ").";
        return false;
    }
    account.SetUuid(StringOf(profile, "id"));
    account.SetProfileName(StringOf(profile, "name"));
    account.SetSkin("", false);
    for (const Json& skin : profile.value("skins", Json::array()))
    {
        if (StringOf(skin, "state") == "ACTIVE")
        {
            account.SetSkin(StringOf(skin, "url"), StringOf(skin, "variant") == "SLIM");
            break;
        }
    }
    return true;
}

bool MSAccountManager::RefreshAccount(MicrosoftAccount& account, std::string& error)
{
    std::string clientId = ClientId();
    if (clientId.empty())
    {
        error = NO_CLIENT_ID;
        return false;
    }

    int status = 0;
    Json token;
    if (!PostForm(MSA_TOKEN_URL, "grant_type=refresh_token&client_id=" + UrlEncode(clientId) + "&refresh_token="
            + UrlEncode(account.GetRefreshToken()) + "&scope=" + UrlEncode(MSA_SCOPE), status, token, error))
    {
        return false;
    }
    if (status != 200)
    {
        error = "Your Microsoft sign-in expired; add the account again.";
        return false;
    }
    if (!StringOf(token, "refresh_token").empty()) account.SetRefreshToken(StringOf(token, "refresh_token"));
    return SignInWithMicrosoftToken(StringOf(token, "access_token"), account, error);
}

std::optional<LoginSession> MSAccountManager::AcquireSession(std::string& error)
{
    MicrosoftAccount account;
    size_t index;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (!currentIndex.has_value())
        {
            error = "No account selected.";
            return std::nullopt;
        }
        index = *currentIndex;
        account = accounts[index];
    }

    if (account.NeedsRefresh(NowSeconds()))
    {
        Log::Info("[AUTH] Refreshing " + account.GetProfileName());
        if (!RefreshAccount(account, error)) return std::nullopt;
        {
            std::lock_guard<std::mutex> lock(accountsMutex);
            if (index < accounts.size() && accounts[index].GetUuid() == account.GetUuid()) accounts[index] = account;
        }
        Save();
    }
    if (!account.GetEntitled())
    {
        error = "This account doesn't own Minecraft: Java Edition.";
        return std::nullopt;
    }

    LoginSession session;
    session.username = account.GetProfileName();
    session.uuid = UuidBytes(account.GetUuid());
    session.accessToken = account.GetAccessToken().raw;
    return session;
}

bool MSAccountManager::JoinServer(const LoginSession& session, const std::string& serverHash, std::string& error)
{
    static const char* hex = "0123456789abcdef";
    std::string uuid;
    for (uint8_t byte : session.uuid)
    {
        uuid += hex[byte >> 4];
        uuid += hex[byte & 15];
    }

    int status = 0;
    Json reply;
    Json request = { { "accessToken", session.accessToken }, { "selectedProfile", uuid }, { "serverId", serverHash } };
    if (!PostJson("https://sessionserver.mojang.com/session/minecraft/join", request, status, reply, error)) return false;
    if (status != 204 && status != 200)
    {
        error = "Mojang's session server refused the login (" + std::to_string(status) + "). Try signing in again.";
        return false;
    }
    return true;
}

std::string MSAccountManager::CurrentSkinUrl(bool& slim)
{
    std::lock_guard<std::mutex> lock(accountsMutex);
    slim = false;
    if (!currentIndex.has_value()) return {};
    slim = accounts[*currentIndex].GetSkinSlim();
    return accounts[*currentIndex].GetSkinUrl();
}

} // namespace Volcano
