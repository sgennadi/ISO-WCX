// Public RIPEMD-160 test vectors from the standard.
#include "iso_wcx/mds_v2.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>

int main() {
    struct Vector { std::string_view text, hash; };
    constexpr std::array<Vector,6> vectors{{
        {"","9c1185a5c5e9fc54612808977ee8f548b2258d31"},
        {"a","0bdc9d2d256b3ee9daae347be6f4dc835a467ffe"},
        {"abc","8eb208f7e05d987a9b044a8e98c6b087f15a0bfc"},
        {"message digest","5d0689ef49d2fae572b881b123a85ffa21595f36"},
        {"abcdefghijklmnopqrstuvwxyz","f71c27109c692c1b56bbdceb5b9d2865b3708dbc"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
         "b0e20b6e3116640286ed3a87a5713079b21f5189"}
    }};
    constexpr char hex[]="0123456789abcdef";
    for(const auto& v:vectors) {
        const auto data=std::span(
            reinterpret_cast<const std::uint8_t*>(v.text.data()),v.text.size());
        const auto hash=iso_wcx::mds_ripemd160(data);
        std::string actual;
        for(const auto b:hash) {
            actual.push_back(hex[b>>4]);actual.push_back(hex[b&15]);
        }
        if(actual!=v.hash)
            throw std::runtime_error("RIPEMD-160 known vector mismatch");
    }
    std::cout<<"PASS: standard RIPEMD-160 digest vectors\n";
}
