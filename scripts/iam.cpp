#include "core/security/NativeIam.h"
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {
// Secrets never enter argv, environment, stdin pipes or query history.
class Secret final {
public:
    std::string value;
    ~Secret() { nexora::security::wipe(value); }
};
class HiddenTerminal final {
#ifdef _WIN32
    HANDLE handle_;
    DWORD original_{};
#else
    termios original_{};
#endif
public:
    HiddenTerminal() {
#ifdef _WIN32
        handle_ = GetStdHandle(STD_INPUT_HANDLE);
        if (!_isatty(_fileno(stdin)) || !GetConsoleMode(handle_,&original_))
            throw std::runtime_error("Interactive terminal required");
        if (!SetConsoleMode(handle_,original_ & ~ENABLE_ECHO_INPUT))
            throw std::runtime_error("Cannot disable terminal echo");
#else
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO,&original_) != 0)
            throw std::runtime_error("Interactive terminal required");
        auto hidden = original_;
        hidden.c_lflag &= ~(ECHO | ICANON | ISIG);
        hidden.c_cc[VMIN] = 1;
        hidden.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO,TCSAFLUSH,&hidden) != 0)
            throw std::runtime_error("Cannot disable terminal echo");
#endif
    }
    ~HiddenTerminal() {
#ifdef _WIN32
        SetConsoleMode(handle_,original_);
#else
        tcsetattr(STDIN_FILENO,TCSAFLUSH,&original_);
#endif
    }
};
void secretInput(const char* prompt, Secret& secret) {
    HiddenTerminal terminal;
    std::cerr << prompt << std::flush;
    // Bound input before allocating; wipe even when input throws.
    for (char c; std::cin.get(c);) {
        if (c == 3 || c == 4) throw std::runtime_error("Secret input cancelled");
        if (c == '\n') { std::cerr << '\n'; return; }
        if (c == 127 || c == 8) {
            if (!secret.value.empty()) { secret.value.back() = 0; secret.value.pop_back(); }
            continue;
        }
        if (secret.value.size() == 1024) throw std::runtime_error("Password exceeds 1024 bytes");
        secret.value.push_back(c);
    }
    throw std::runtime_error("Secret input interrupted");
}
}
int main(int argc, char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--version") {
            std::cout << "nexora_iam " << NEXORA_PROJECT_VERSION << '\n';return 0;
        }
        if(argc==2 && (std::string(argv[1])=="--help" || std::string(argv[1])=="-h")) {
            std::cout << "Usage: nexora_iam <init|adopt|status|recover> <database-directory>\n"
                         "Passwords and factors are read through hidden local terminal input.\n";return 0;
        }
        if (argc != 3 || (std::string(argv[1]) != "init" && std::string(argv[1]) != "status" &&
            std::string(argv[1]) != "recover" && std::string(argv[1]) != "adopt")) {
            std::cerr << "Usage: nexora_iam <init|adopt|status|recover> <database-directory>\n";
            return 2;
        }
        nexora::security::OfflineIdentityStore store(argv[2]);
        const auto state = store.status();
        if (std::string(argv[1]) == "status") {
            std::cout << (state.initialized ? "INITIALIZED" : "UNINITIALIZED") << '\n';
            if (state.initialized) std::cout << "Domain: " << state.domain_id << "\nRoot ID: "
                << state.root_id << "\nUsername: " << state.username << '\n';
            return 0;
        }
        if (std::string(argv[1]) == "recover") {
            if(!state.initialized) throw std::runtime_error("Initialize the owner locally before recovery");
            Secret factor,password,confirmation;
            secretInput("Offline recovery factor: ",factor);
            secretInput("New password (at least 15 bytes): ",password);
            secretInput("Confirm password: ",confirmation);
            if(password.value!=confirmation.value) throw std::runtime_error("Passwords do not match");
            store.recoverRoot(factor.value,password.value);
            std::cout<<"Owner password recovered. Factor consumed; old sessions and delegated credentials revoked.\n";
            return 0;
        }
        if (state.initialized) throw std::runtime_error("Bootstrap is permanently closed");
        const bool adopting=std::string(argv[1])=="adopt";
        Secret previous;
        if(adopting) {
            std::cerr<<"Stop the service and create a verified backup first. Existing legacy credentials will be revoked.\n"
                        "Type ADOPT EXISTING DATA to migrate this store: "<<std::flush;
            std::string confirmation;std::getline(std::cin,confirmation);
            if(confirmation!="ADOPT EXISTING DATA") throw std::runtime_error("Migration confirmation did not match");
            secretInput("Previous legacy owner password (empty only if no legacy owner exists): ",previous);
        }
        std::cerr << "Owner enrollment is local only. Online access requires the native authenticated gateway.\n"
                     "Owner username: " << std::flush;
        std::string username;
        for (char c; std::cin.get(c);) {
            if (c == '\n') break;
            if (username.size() == 64) throw std::runtime_error("Username exceeds 64 characters");
            username.push_back(c);
        }
        Secret password, confirmation;
        secretInput("New password (at least 15 bytes): ", password);
        secretInput("Confirm password: ", confirmation);
        if (password.value != confirmation.value) throw std::runtime_error("Passwords do not match");
        auto receipt = adopting?store.adoptExisting(username,password.value,"ADOPT EXISTING DATA",previous.value):
            store.initialize(username,password.value);
        nexora::security::wipe(password.value);
        nexora::security::wipe(confirmation.value);
        std::cout << "Owner enrolled. Save these single-use recovery factors offline.\n"
                     "They cannot be retrieved later. Use nexora_iam recover only while the service is stopped.\n";
        for (auto& code : receipt.recovery_codes) {
            std::cout << code << '\n';
            nexora::security::wipe(code);
        }
        std::cout << std::flush;
        if (!std::cout) throw std::runtime_error("Enrollment committed but recovery output failed; do not retry bootstrap");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "IAM: " << error.what() << '\n';
        return 1;
    }
}
