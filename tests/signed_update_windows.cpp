#ifdef _WIN32
#include "iso_wcx/signed_update.hpp"
#include <bcrypt.h>
#include <array>
#include <stdexcept>
#define assert(condition) do { if(!(condition)) throw std::runtime_error(#condition); } while(false)
#include <cstdint>
#include <cstring>
#include <vector>
#pragma comment(lib,"bcrypt.lib")
int main(){
    using namespace iso_wcx::signed_update;
    BCRYPT_ALG_HANDLE ec=nullptr, sha=nullptr;
    BCRYPT_KEY_HANDLE key=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    assert(BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&ec,BCRYPT_ECDSA_P256_ALGORITHM,nullptr,0)));
    assert(BCRYPT_SUCCESS(BCryptGenerateKeyPair(ec,&key,256,0)));
    assert(BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key,0)));
    ULONG size=0;
    assert(BCRYPT_SUCCESS(BCryptExportKey(key,nullptr,BCRYPT_ECCPUBLIC_BLOB,nullptr,0,&size,0)));
    std::vector<UCHAR> blob(size);
    assert(BCRYPT_SUCCESS(BCryptExportKey(key,nullptr,BCRYPT_ECCPUBLIC_BLOB,blob.data(),size,&size,0)));
    assert(size==sizeof(BCRYPT_ECCKEY_BLOB)+64);
    std::array<std::uint8_t,64> pub{};
    std::memcpy(pub.data(),blob.data()+sizeof(BCRYPT_ECCKEY_BLOB),64);
    const std::array<std::uint8_t,18> message{'I','S','O','-','W','C','X',' ','s','i','g','n','e','d',' ','t','e','s'};
    assert(BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&sha,BCRYPT_SHA256_ALGORITHM,nullptr,0)));
    assert(BCRYPT_SUCCESS(BCryptCreateHash(sha,&hash,nullptr,0,nullptr,0,0)));
    assert(BCRYPT_SUCCESS(BCryptHashData(hash,const_cast<PUCHAR>(message.data()),static_cast<ULONG>(message.size()),0)));
    std::array<UCHAR,32> digest{};
    assert(BCRYPT_SUCCESS(BCryptFinishHash(hash,digest.data(),32,0)));
    std::array<UCHAR,64> signature{};
    ULONG written=0;
    assert(BCRYPT_SUCCESS(BCryptSignHash(key,nullptr,digest.data(),32,signature.data(),64,&written,0)));
    assert(written==64);
    assert(verify_p256_sha256(message,signature,pub));
    auto changed=message;changed[0]^=1;
    assert(!verify_p256_sha256(changed,signature,pub));
    signature[0]^=1;
    assert(!verify_p256_sha256(message,signature,pub));
    assert(!verify_p256_sha256(message,std::span<const std::uint8_t>(signature.data(),63),pub));
    std::array<std::uint8_t,64> decoded{};
    assert(!decode_public_key("",decoded));
    assert(!decode_public_key(std::string(128,'z'),decoded));
    BCryptDestroyHash(hash);
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(sha,0);
    BCryptCloseAlgorithmProvider(ec,0);
    return 0;
}
#endif
