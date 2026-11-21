#pragma once
// Windows-only ECDSA P-256 / SHA-256 signature verification.
// Signature asset is exactly 64 raw bytes: r[32] || s[32] (big endian).
// The pinned public key is exactly 64 raw bytes: x[32] || y[32].
// Never retrieve the public key from the same release as the ZIP.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <algorithm>

namespace iso_wcx::signed_update {
inline bool verify_p256_sha256(std::span<const std::uint8_t> manifest,
                              std::span<const std::uint8_t> signature,
                              std::span<const std::uint8_t> public_xy) noexcept {
    if(manifest.empty() || manifest.size()>16384 ||
       signature.size()!=64 || public_xy.size()!=64)return false;
    BCRYPT_ALG_HANDLE hash_alg=nullptr, curve_alg=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    BCRYPT_KEY_HANDLE key=nullptr;
    bool valid=false;
    do {
        if(!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hash_alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)))break;
        if(!BCRYPT_SUCCESS(BCryptCreateHash(hash_alg,&hash,nullptr,0,nullptr,0,0)))break;
        if(!BCRYPT_SUCCESS(BCryptHashData(hash,const_cast<PUCHAR>(manifest.data()),
                                         static_cast<ULONG>(manifest.size()),0)))break;
        std::array<UCHAR,32> digest{};
        if(!BCRYPT_SUCCESS(BCryptFinishHash(hash,digest.data(),32,0)))break;
        if(!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&curve_alg,BCRYPT_ECDSA_P256_ALGORITHM,nullptr,0)))break;
        std::array<std::uint8_t,sizeof(BCRYPT_ECCKEY_BLOB)+64> blob{};
        BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC,32};
        std::copy_n(reinterpret_cast<const std::uint8_t*>(&header),sizeof(header),blob.data());
        std::copy(public_xy.begin(),public_xy.end(),blob.begin()+sizeof(header));
        if(!BCRYPT_SUCCESS(BCryptImportKeyPair(curve_alg,nullptr,BCRYPT_ECCPUBLIC_BLOB,
                     &key,blob.data(),static_cast<ULONG>(blob.size()),0)))break;
        valid=BCRYPT_SUCCESS(BCryptVerifySignature(key,nullptr,digest.data(),32,
                     const_cast<PUCHAR>(signature.data()),64,0));
    }while(false);
    if(key)BCryptDestroyKey(key);
    if(hash)BCryptDestroyHash(hash);
    if(curve_alg)BCryptCloseAlgorithmProvider(curve_alg,0);
    if(hash_alg)BCryptCloseAlgorithmProvider(hash_alg,0);
    return valid;
}
inline bool decode_public_key(std::string_view hex,std::array<std::uint8_t,64>& out) noexcept {
    if(hex.size()!=128)return false;
    auto digit=[](char c)->int {
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;
        return -1;
    };
    for(std::size_t i=0;i<64;++i){
        int a=digit(hex[2*i]),b=digit(hex[2*i+1]);
        if(a<0||b<0)return false;
        out[i]=static_cast<std::uint8_t>((a<<4)|b);
    }
    return true;
}
} // namespace iso_wcx::signed_update
#endif
