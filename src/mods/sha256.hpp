#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rocket::mods {
inline std::string sha256(std::span<const std::uint8_t> input) {
    constexpr std::array<std::uint32_t,64> k{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<std::uint32_t,8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    // Process directly from the input; only the final one/two blocks need padding.
    const std::size_t blocks = (input.size() + 9 + 63) / 64;
    const std::uint64_t bits = static_cast<std::uint64_t>(input.size()) * 8;
    for (std::size_t block=0; block<blocks; ++block) {
        std::array<std::uint32_t,64> w{};
        for (std::size_t i=0; i<64; ++i) {
            const auto pos=block*64+i;
            std::uint8_t byte=0;
            if (pos<input.size()) byte=input[pos];
            else if (pos==input.size()) byte=0x80;
            else if (pos>=blocks*64-8) byte=static_cast<std::uint8_t>(bits >> ((blocks*64-1-pos)*8));
            w[i/4] |= static_cast<std::uint32_t>(byte) << (24-(i%4)*8);
        }
        for (std::size_t i=16;i<64;++i) {
            const auto s0=std::rotr(w[i-15],7)^std::rotr(w[i-15],18)^(w[i-15]>>3);
            const auto s1=std::rotr(w[i-2],17)^std::rotr(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        auto [a,b,c,d,e,f,g,z]=h;
        for (std::size_t i=0;i<64;++i) {
            const auto s1=std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25);
            const auto t1=z+s1+((e&f)^(~e&g))+k[i]+w[i];
            const auto s0=std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22);
            const auto t2=s0+((a&b)^(a&c)^(b&c));
            z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        const std::array<std::uint32_t,8> v{a,b,c,d,e,f,g,z};
        for (std::size_t i=0;i<8;++i) h[i]+=v[i];
    }
    constexpr char hex[]="0123456789abcdef";
    std::string out; out.reserve(64);
    for (auto word:h) for (int shift=28;shift>=0;shift-=4) out+=hex[(word>>shift)&15];
    return out;
}
}

