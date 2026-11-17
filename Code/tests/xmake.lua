
target("TPTests")
    set_kind("binary")
    set_group("Tests")
    add_includedirs(
        ".", "..", "../common", "../encoding", "../client")
    add_headerfiles("**.h")
    add_files("*.cpp")
    -- Has no game dependencies, so the client's remote playback timing can be tested here
    add_files("../client/Systems/PlaybackClock.cpp")
    add_deps("SkyrimEncoding", "CommonLib")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "catch2",
        "mimalloc",
        "glm")
