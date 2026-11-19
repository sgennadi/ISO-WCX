#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <utility>

namespace iso_wcx::wim_scratch {

// SetSecurityInfo can report ERROR_ACCESS_DENIED when applying a protected
// DACL to NTFS directories containing WIMGAPI-applied children. Windows
// automatically propagates inheritable ACE changes on this API. Cleanup
// only needs access to the SINGLE disposable scratch inode; try a plain,
// non-inheritable DACL before changing its owner. No operation on the image,
// the parent directory, other hardlinks, or a WIMMount registration is allowed.
struct AclRecovery {
    static constexpr DWORD unused=0xffffffffu;
    DWORD protected_first=unused;
    DWORD plain_first=unused;
    DWORD owner_change=unused;
    DWORD plain_after_owner=unused;
    DWORD protected_after_owner=unused;
    DWORD reopened_plain=unused;
    DWORD reopened_protected=unused;
    DWORD final_error=ERROR_SUCCESS;

    [[nodiscard]] bool ok() const noexcept {
        return final_error==ERROR_SUCCESS;
    }
    [[nodiscard]] std::string counts() const {
        const auto fmt=[](DWORD code) {
            return code==unused?std::string("not-tried"):
                std::to_string(code);
        };
        return " (protected_first="+fmt(protected_first)+
            ", plain_first="+fmt(plain_first)+
            ", owner_change="+fmt(owner_change)+
            ", plain_after_owner="+fmt(plain_after_owner)+
            ", protected_after_owner="+fmt(protected_after_owner)+
            ", reopened_plain="+fmt(reopened_plain)+
            ", reopened_protected="+fmt(reopened_protected)+")";
    }
};

// Testable decision tree. The supplied callbacks MUST already be restricted
// to an opened, singly-linked private scratch inode. If Windows still denies
// the DACL change, fail closed and report numeric stages; never ignore it.
template<class ApplyDacl,class SetOwner,class ReopenDacl>
AclRecovery recover_private_scratch_acl(
    ApplyDacl&& apply,SetOwner&& set_owner,ReopenDacl&& reopen) {
    AclRecovery result;
    result.protected_first=apply(true);
    if(result.protected_first==ERROR_SUCCESS)return result;
    result.final_error=result.protected_first;
    if(result.protected_first!=ERROR_ACCESS_DENIED &&
       result.protected_first!=ERROR_INVALID_PARAMETER)return result;

    // The new ACL has NO_INHERITANCE; no descendant rights are required.
    // Avoid needless ownership changes when a plain DACL succeeds.
    result.plain_first=apply(false);
    result.final_error=result.plain_first;
    if(result.ok() || result.plain_first!=ERROR_ACCESS_DENIED)return result;

    result.owner_change=set_owner();
    result.final_error=result.owner_change;
    if(!result.ok())return result;

    // Windows may evaluate ACL access under the new owner differently.
    result.plain_after_owner=apply(false);
    result.final_error=result.plain_after_owner;
    if(result.ok())return result;

    result.protected_after_owner=apply(true);
    result.final_error=result.protected_after_owner;
    if(result.ok())return result;
    if(result.plain_after_owner!=ERROR_ACCESS_DENIED &&
       result.protected_after_owner!=ERROR_ACCESS_DENIED)return result;

    // A new handle re-evaluates access after the owner transition. Reopen
    // callback must verify the NTFS file ID before any further DACL change.
    result.reopened_plain=reopen(false);
    result.final_error=result.reopened_plain;
    if(result.ok())return result;
    if(result.reopened_plain!=ERROR_ACCESS_DENIED)return result;
    result.reopened_protected=reopen(true);
    result.final_error=result.reopened_protected;
    return result;
}

} // namespace iso_wcx::wim_scratch
#endif
