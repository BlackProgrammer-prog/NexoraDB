#include "core/security/NativeIam.h"
#include "RecoveryTestSupport.h"
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {
void killSelf() noexcept { std::raise(SIGKILL); std::_Exit(90); }
}
int main(int argc, char** argv) {
    try {
        recovery_test::require(argc == 4,"usage: iam_crash_probe path action stage");
        const std::string action = argv[2];
        if (action == "verify") {
            nexora::security::OfflineIdentityStore store(argv[1]);
            const bool initialized = store.status().initialized;
            const std::string expectation = argv[3];
            recovery_test::require(expectation == "either" || expectation == "either-or-locked" || initialized == (expectation == "after"),
                "unexpected bootstrap state");
            if (initialized) {
                bool rejected = false;
                try { store.initialize("second_owner","another long enough password"); }
                catch (const std::runtime_error&) { rejected = true; }
                recovery_test::require(rejected,"bootstrap reopened after restart");
            } else {
                // An all-before crash must permit exactly one complete retry.
                store.initialize("retried_owner","correct horse battery staple");
                recovery_test::require(store.status().initialized,"crash retry did not commit");
            }
            std::cout << (initialized ? "after" : "before") << std::endl;
            return 0;
        }
        recovery_test::FaultEnv env;
        nexora::security::OfflineIdentityStore store(argv[1],&env);
        const int stage = std::stoi(argv[3]);
        if (action == "partial") {
            env.failure = stage;
            bool rejected = false;
            try { store.initialize("chosen_owner","correct horse battery staple"); }
            catch (const std::runtime_error&) { rejected = true; }
            recovery_test::require(rejected && env.hits > 0,"WAL error received a success ACK");
            bool latched = false;
            try { store.initialize("second_owner","correct horse battery staple"); }
            catch (const std::runtime_error&) { latched = true; }
            recovery_test::require(latched,"failed commit allowed an in-process retry");
            killSelf();
        }
        env.crash_stage = stage;
        env.crash = killSelf;
        store.initialize("chosen_owner","correct horse battery staple");
        if (stage == 5) { std::cout << "COMMITTED" << std::endl; killSelf(); }
        throw std::runtime_error("crash boundary was not reached");
    } catch (const std::exception& error) {
        // A deliberately truncated physical WAL may lock the store under its
        // strict recovery policy. Accept only this precise corruption failure,
        // and independently require that the legacy engine cannot open it.
        if (argc == 4 && std::string(argv[2]) == "verify" &&
            std::string(argv[3]) == "either-or-locked" &&
            std::string(error.what()).find("Corruption:") != std::string::npos) {
            try { nexora::core::DocEngine legacy(argv[1]); }
            catch (const std::runtime_error& denied) {
                if (std::string(denied.what()).find("Corruption:") != std::string::npos) {
                    std::cout << "locked (physical WAL corruption)" << std::endl;
                    return 0;
                }
            }
        }
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
