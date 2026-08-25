#pragma once
#include <string>
#include <vector>
#include <cstddef>

enum class FnexCryptoKind {
    Ini,
    Ez,
    Ezi
};

// Implements the 2013 ez2djDecoder/Encode-FNEX transform internally.
// This is the inverse of the game's runtime unpackers that create .int/.es/.ezz.
std::vector<unsigned char> FnexEncryptBuffer(const unsigned char* data, size_t size, FnexCryptoKind kind);
std::vector<unsigned char> FnexEncryptString(const std::string& text, FnexCryptoKind kind);
bool FnexWriteEncryptedFile(const std::string& path, const unsigned char* data, size_t size, FnexCryptoKind kind);
bool FnexWriteEncryptedTextFile(const std::string& path, const std::string& text, FnexCryptoKind kind);
const char* FnexCryptoKindName(FnexCryptoKind kind);
