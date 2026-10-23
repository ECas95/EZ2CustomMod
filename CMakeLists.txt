cmake_minimum_required(VERSION 3.20)
project(EZ2CustomMod LANGUAGES CXX)

add_library(EZ2CustomMod SHARED
    src/mod_main.cpp
    src/config.cpp
    src/logger.cpp
    src/patch.cpp
    src/iat.cpp
    src/window_hook.cpp
    src/bms_scanner.cpp
    src/audio_decode.cpp
    src/fnex_crypto.cpp
    src/trace_hooks.cpp
    src/progress_popup.cpp
)

target_compile_features(EZ2CustomMod PRIVATE cxx_std_17)
target_compile_definitions(EZ2CustomMod PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# Media Foundation is resolved at runtime so the DLL still loads when it is absent (XP).
target_link_libraries(EZ2CustomMod PRIVATE user32 kernel32 shell32 comctl32 gdi32 gdiplus ole32 mfuuid)

if(MSVC)
    target_compile_options(EZ2CustomMod PRIVATE /W4 /EHsc)
else()
    target_compile_options(EZ2CustomMod PRIVATE -Wall -Wextra)
    target_link_options(EZ2CustomMod PRIVATE -static-libgcc -static-libstdc++ -no-pthread)
    target_link_libraries(EZ2CustomMod PRIVATE -Wl,-Bstatic -Wl,--whole-archive winpthread -Wl,--no-whole-archive -Wl,-Bdynamic)
endif()
