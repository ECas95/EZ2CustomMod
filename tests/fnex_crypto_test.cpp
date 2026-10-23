// FnexDecryptBuffer undoes FnexEncryptBuffer, and reads a real game .ini (src/fnex_crypto.cpp).
#include "../src/fnex_crypto.cpp"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    // Every block-walk edge: under 8, exactly 8, the 8/16 alternation, and a long tail.
    for (size_t size : {0u, 1u, 7u, 8u, 9u, 23u, 24u, 25u, 40u, 1000u, 4099u}) {
        std::vector<unsigned char> plain(size);
        for (size_t i = 0; i < size; ++i) plain[i] = (unsigned char)(i * 37 + 11);
        const auto enc = FnexEncryptBuffer(plain.data(), plain.size(), FnexCryptoKind::Ini);
        const auto dec = FnexDecryptBuffer(enc.data(), enc.size(), FnexCryptoKind::Ini);
        assert(dec == plain);
    }
    // The shipped 5RadioMix course list, when the game is next to the source tree.
    FILE* f = fopen("EZ2AC_FNEX\\system\\5radiomix\\stage.ini", "rb");
    if (f) {
        std::vector<unsigned char> blob;
        int c;
        while ((c = fgetc(f)) != EOF) blob.push_back((unsigned char)c);
        fclose(f);
        const auto dec = FnexDecryptBuffer(blob.data(), blob.size(), FnexCryptoKind::Ini);
        const std::string text(dec.begin(), dec.end());
        assert(text.find("\"channel_normal\"") != std::string::npos);
        assert(FnexEncryptBuffer(dec.data(), dec.size(), FnexCryptoKind::Ini) == blob);
    }
    std::puts("fnex_crypto_test OK");
    return 0;
}
