#include "RecoveryTestSupport.h"
#include <iostream>
#include <csignal>
#include <filesystem>
using namespace recovery_test;
namespace {
void killSelf() noexcept {
#ifdef SIGKILL
    std::raise(SIGKILL);
#endif
    std::_Exit(90);
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 5, "usage: probe path action mode stage/policy");
        const std::string path = argv[1], action = argv[2], mode = argv[3], policy = argv[4];
        FaultEnv env;
        if (action == "seed") {
            require(!std::filesystem::exists(path), "seed requires fresh owned path");
            DocEngine db(path, {}, settings()); seed(db); verify(db, expected(mode, false));
        } else if (action == "partial") {
            DocEngine db(path, {}, settings(&env)); db.PauseDocumentMigration();
            env.failure = 4;
            const auto result = mutate(db, mode);
            require(!result.success && result.error_code == "io_error" && env.hits > 0,
                    "partial WAL write received a success ACK or missed injection");
            require(!db.IsHealthy() && !db.PrepareShutdown().success, "partial WAL error was not latched");
            std::cout << "COR007_NACK" << std::endl;
            killSelf();
        } else if (action == "crash") {
            DocEngine db(path, {}, settings(&env)); db.PauseDocumentMigration();
            const int stage = std::stoi(policy);
            if (stage <= 3) db.SetMutationFaultPointForTesting(static_cast<MutationFaultPoint>(stage), killSelf);
            else if (stage <= 7) { env.crash_stage = stage - 3; env.crash = killSelf; }
            checked(mutate(db, mode));
            if (stage == 8) { std::cout << "COR007_ACK" << std::endl; killSelf(); }
            throw std::runtime_error("selected crash boundary was never reached");
        } else if (action == "verify") {
            DocEngine db(path, {}, settings()); db.PauseDocumentMigration();
            const auto rows = snapshot(db);
            const bool before = rows == expected(mode, false), after = rows == expected(mode, true);
            require(policy == "before" ? before : policy == "after" ? after : before || after, "partial or lost committed state");
            verify(db, rows);
            std::cout << Json{{"recovery", after ? "after" : "before"}, {"documents", rows.size()},
                              {"oracle", "document/index/counter/page/point/integrity"}} << std::endl;
        } else if (action == "reject") {
            bool rejected = false;
            try {
                DocEngine db(path, {}, settings()); db.PauseDocumentMigration();
                const auto integrity = db.IntegrityCheck();
                rejected = !integrity.success;
                if (rejected) std::cout << Json{{"recovery", "integrity_error"}, {"error", integrity.error_msg}} << std::endl;
            } catch (const std::exception& error) {
                // An unrelated open failure must not masquerade as corruption coverage.
                rejected = std::string(error.what()).find("Corruption:") != std::string::npos;
                std::cout << Json{{"recovery", "open_error"}, {"error", error.what()}} << std::endl;
            }
            require(rejected, "physical corruption was silently accepted");
        } else throw std::runtime_error("unknown action");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}
