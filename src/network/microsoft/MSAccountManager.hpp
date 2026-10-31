#pragma once
#ifndef VOLCANO_NETWORK_MSA_MANAGER_H
#define VOLCANO_NETWORK_MSA_MANAGER_H

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "MicrosoftAccount.hpp"

namespace Volcano {

class Config;

// What a connection needs to log in as an account.
struct LoginSession
{
    std::string username;
    std::array<uint8_t, 16> uuid{};
    std::string accessToken;
};

// Progress of "Add account", for the sign-in screen.
struct DeviceLoginState
{
    enum class Phase { Idle, WaitingForCode, WaitingForUser, SigningIn, Done, Failed };
    Phase phase = Phase::Idle;
    std::string userCode;        // Typed in at verificationUri.
    std::string verificationUri;
    std::string error;
};

// The signed-in accounts, which one is current (the account switcher's choice), and the Microsoft -> Xbox -> Minecraft
// sign-in chain. Accounts persist in accounts.json (refresh tokens only; access tokens are rebuilt when needed).
// Every method is safe from any thread; HTTP-doing ones block, so keep them off the render thread.
class MSAccountManager
{
public:
    explicit MSAccountManager(Config* config);
    ~MSAccountManager();

    void Load();

    size_t Count();
    std::optional<size_t> CurrentIndex();   // Empty means offline mode.
    void SetCurrent(std::optional<size_t> index);
    void Remove(size_t index);

    // Copies for the switcher, so the GUI never holds the lock.
    std::vector<MicrosoftAccount> Snapshot();

    // Starts the device-code sign-in on a worker thread; poll GetDeviceLogin().
    void BeginDeviceLogin();
    void CancelDeviceLogin();
    DeviceLoginState GetDeviceLogin();

    // Refreshes the current account if its token is stale and returns its login. Empty (with `error`) if there is no
    // current account or the sign-in failed. Blocking.
    std::optional<LoginSession> AcquireSession(std::string& error);

    // Tells Mojang's session server this client is joining `serverHash`. Blocking.
    bool JoinServer(const LoginSession& session, const std::string& serverHash, std::string& error);

    // The current account's skin, for the local player; empty URL when offline or it has none.
    std::string CurrentSkinUrl(bool& slim);

private:
    bool RefreshAccount(MicrosoftAccount& account, std::string& error);
    bool SignInWithMicrosoftToken(const std::string& msaAccessToken, MicrosoftAccount& account, std::string& error);
    void RunDeviceLogin(std::stop_token stopToken);
    void Save();
    void SetDeviceLogin(DeviceLoginState::Phase phase, const std::string& error = "");

    Config* config;
    std::string ClientId();

    std::mutex accountsMutex;
    std::vector<MicrosoftAccount> accounts;
    std::optional<size_t> currentIndex;

    std::mutex deviceLoginMutex;
    DeviceLoginState deviceLogin;
    std::jthread deviceThread;
};

} // namespace Volcano

#endif
