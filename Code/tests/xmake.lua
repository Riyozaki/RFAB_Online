target("TPTests")
    set_kind("binary")
    set_group("Tests")
    add_includedirs(
        ".", "..", "../common", "../encoding", "../server")
    add_headerfiles("**.h")
    add_files("*.cpp")
    add_deps("SkyrimEncoding", "CommonLib", "TiltedConnect")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "spdlog",
        "catch2",
        "mimalloc",
        "glm",
        "entt",
        "gamenetworkingsockets",
        "libuv")
