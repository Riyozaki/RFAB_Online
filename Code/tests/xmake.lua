target("TPTests")
    set_kind("binary")
    set_group("Tests")
    add_includedirs(
        ".", "..", "../common", "../encoding", "../server")
    add_headerfiles("**.h")
    add_files("*.cpp")
    add_deps("SkyrimEncoding", "CommonLib", "TiltedConnect", "ESLoader")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "spdlog",
        "catch2",
        "mimalloc",
        "glm",
        "entt",
        "zlib",
        "gamenetworkingsockets",
        "libuv")
    if is_plat("windows") then
        add_syslinks("ws2_32", "iphlpapi", "crypt32", "winmm", "bcrypt", "user32", "advapi32", "userenv", "psapi", "dbghelp")
    end
