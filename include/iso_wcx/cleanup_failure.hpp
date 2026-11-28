#pragma once
// Keep only a bounded, path-free cleanup failure for the current WCX thread.
// An Operation emits it once on completion, even when a noexcept destructor
// cannot propagate a cleanup exception. Never retain exception::what(), which
// may contain a user name, private image path or WIM filename.
#include <exception>
#include <filesystem>
#include <system_error>

namespace iso_wcx::cleanup_diagnostics {
enum class Stage { transaction, wim_workspace, wim_expanded };
struct Failure {
    Stage stage=Stage::transaction;
    const char* kind="none";
    int os_code=0;
    unsigned attempts=0;
    bool present=false;
};
inline thread_local Failure last_failure{};

inline void reset() noexcept { last_failure={}; }
inline const char* stage_name(Stage stage) noexcept {
    switch(stage) {
        case Stage::transaction: return "transaction";
        case Stage::wim_workspace: return "wim_workspace";
        case Stage::wim_expanded: return "wim_expanded";
    }
    return "unknown";
}
inline void note(Stage stage, std::error_code error,
                 unsigned attempts=1) noexcept {
    last_failure={stage,"filesystem_error",error.value(),attempts,true};
}
inline void note_remaining(Stage stage,unsigned attempts=1) noexcept {
    last_failure={stage,"still_present",0,attempts,true};
}
inline void note(Stage stage,std::exception_ptr error,
                 unsigned attempts=1) noexcept {
    if(!error)return;
    try {
        std::rethrow_exception(error);
    }catch(const std::filesystem::filesystem_error& e) {
        note(stage,e.code(),attempts);
    }catch(const std::system_error& e) {
        last_failure={stage,"system_error",e.code().value(),attempts,true};
    }catch(const std::exception&) {
        last_failure={stage,"exception",0,attempts,true};
    }catch(...) {
        last_failure={stage,"unknown_exception",0,attempts,true};
    }
}
inline Failure take() noexcept {
    const Failure result=last_failure;
    reset();
    return result;
}
} // namespace iso_wcx::cleanup_diagnostics
