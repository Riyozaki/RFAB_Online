target("CommonLib")
    on_load(function (target)
        local version_file = path.join(os.scriptdir(), "..", "..", "VERSION")
        local semver = "0.0.0"
        if os.isfile(version_file) then
            semver = io.readfile(version_file):trim()
        end
        local expected_tag = "v" .. semver
        local build_ver = expected_tag
        try
        {
            function ()
                import("detect.tools.find_git")
                local git = find_git()
                if git then
                    local exact = try { function () return os.iorunv(git, {"describe", "--tags", "--exact-match", "HEAD"}):trim() end }
                    if exact == expected_tag then
                        build_ver = expected_tag
                    else
                        local sha = os.iorunv(git, {"rev-parse", "--short", "HEAD"}):trim()
                        if sha and #sha > 0 then
                            build_ver = expected_tag .. "-g" .. sha
                        end
                    end
                end
            end
        }
        target:set("configvar", "RFAB_BUILD_VERSION", build_ver)
    end)
    add_configfiles("BuildInfo.h.in")
    set_kind("static")
    set_group("common")
    add_includedirs(".", "../", "../../build", {public = true})
    add_headerfiles("**.h")
    add_files("**.cpp")
    add_packages("tiltedcore", "hopscotch-map")
