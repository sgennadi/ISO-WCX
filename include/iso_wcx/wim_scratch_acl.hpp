#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <utility>

namespace iso_wcx::wim_scratch {

// SetSecurityInfo on an ordinary file handle can impose NTFS automatic
// inheritance rules on EXISTING children even when the new DACL has no
// inheritable ACEs. Under applied Windows-image ACLs, a protected child can
// make the PARENT's DACL update return ACCESS_DENIED after owner_change=0.
// Microsoft's SetSecurityInfo contract explicitly disables propagation when
// the handle was OPENED with an ACCESS_MASK of MAXIMUM_ALLOWED.
//
// All callbacks operate on a single, inode-verified private scratch entry;
// never on the source WIM, parent of the workspace or WIMMount. The
// MAXIMUM_ALLOWED callback must use FILE_FLAG_OPEN_REPARSE_POINT and verify
// the same volume+file index and single-link count before SetSecurityInfo.
struct AclRecovery {
    static constexpr DWORD unused=0xffffffffu;
    DWORD protected_first=unused;
    DWORD plain_first=unused;
    DWORD max_before_plain=unused;
    DWORD max_before_protected=unused;
    DWORD owner_change=unused;
    DWORD plain_after_owner=unused;
    DWORD protected_after_owner=unused;
    DWORD reopened_plain=unused;
    DWORD reopened_protected=unused;
    DWORD max_after_plain=unused;
    DWORD max_after_protected=unused;
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
            ", max_before_plain="+fmt(max_before_plain)+
            ", max_before_protected="+fmt(max_before_protected)+
            ", owner_change="+fmt(owner_change)+
            ", plain_after_owner="+fmt(plain_after_owner)+
            ", protected_after_owner="+fmt(protected_after_owner)+
            ", reopened_plain="+fmt(reopened_plain)+
            ", reopened_protected="+fmt(reopened_protected)+
            ", max_after_plain="+fmt(max_after_plain)+
            ", max_after_protected="+fmt(max_after_protected)+")";
    }
};

// The special MAXIMUM_ALLOWED path must never be used outside an inode-
// verified, singly-linked and non-reparse private WIM staging entry. Bail
// immediately on unexpected Win32 codes (including file ID mismatch) rather
// than continuing to change the owner of a possibly replaced path.
template<class ApplyDacl,class SetOwner,class ReopenDacl,
         class ApplyWithoutPropagation>
AclRecovery recover_private_scratch_acl(
    ApplyDacl&& apply,SetOwner&& set_owner,ReopenDacl&& reopen,
    ApplyWithoutPropagation&& no_propagation) {
    AclRecovery result;
    result.protected_first=apply(true);
    result.final_error=result.protected_first;
    if(result.ok())return result;
    if(result.protected_first!=ERROR_ACCESS_DENIED &&
       result.protected_first!=ERROR_INVALID_PARAMETER)return result;

    result.plain_first=apply(false);
    result.final_error=result.plain_first;
    if(result.ok() || result.plain_first!=ERROR_ACCESS_DENIED)return result;

    // FIRST attempt single-inode DACL updates without ownership changes.
    // Windows documentation guarantees no child ACE propagation when
    // SetSecurityInfo uses a handle opened with MAXIMUM_ALLOWED.
    result.max_before_plain=no_propagation(false);
    result.final_error=result.max_before_plain;
    if(result.ok() || result.max_before_plain!=ERROR_ACCESS_DENIED)
        return result;
    result.max_before_protected=no_propagation(true);
    result.final_error=result.max_before_protected;
    if(result.ok() || result.max_before_protected!=ERROR_ACCESS_DENIED)
        return result;

    result.owner_change=set_owner();
    result.final_error=result.owner_change;
    if(!result.ok())return result;

    result.plain_after_owner=apply(false);
    result.final_error=result.plain_after_owner;
    if(result.ok())return result;

    result.protected_after_owner=apply(true);
    result.final_error=result.protected_after_owner;
    if(result.ok())return result;
    if(result.plain_after_owner!=ERROR_ACCESS_DENIED &&
       result.protected_after_owner!=ERROR_ACCESS_DENIED)return result;

    result.reopened_plain=reopen(false);
    result.final_error=result.reopened_plain;
    if(result.ok())return result;
    if(result.reopened_plain!=ERROR_ACCESS_DENIED)return result;

    result.reopened_protected=reopen(true);
    result.final_error=result.reopened_protected;
    if(result.ok() || result.reopened_protected!=ERROR_ACCESS_DENIED)
        return result;

    // If the owner is now correct but Windows still refuses ACL writes,
    // retry without directory inheritance propagation on a *fresh* verified
    // MAXIMUM_ALLOWED handle. A failed attempt never counts as cleanup.
    result.max_after_plain=no_propagation(false);
    result.final_error=result.max_after_plain;
    if(result.ok() || result.max_after_plain!=ERROR_ACCESS_DENIED)
        return result;
    result.max_after_protected=no_propagation(true);
    result.final_error=result.max_after_protected;
    return result;
}

} // namespace iso_wcx::wim_scratch
#endif
