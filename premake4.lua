local qt = require 'qt'

local dependencies = {
    dvs_display = {'background_cleaner'},
    blob_display = {'background_cleaner'},
    flow_display = {'background_cleaner'},
    frame_generator = {'grey_display'},
}
setmetatable(dependencies, {__index = function() return {} end})

solution 'chameleon'
    configurations {'release', 'debug'}
    location 'build'
    for index, file in pairs(os.matchfiles('test/*.cpp')) do
        local name = path.getbasename(file)
        project(name)
            kind 'ConsoleApp'
            language 'C++'
            location 'build'
            files {'source/' .. name .. '.hpp', 'test/' .. name .. '.cpp'}
            files(qt.moc({'source/' .. name .. '.hpp'}, 'build/moc'))
            for index, dependency_name in pairs(dependencies[name]) do
                files(qt.moc({'source/' .. dependency_name .. '.hpp'}, 'build/moc'))
            end
            includedirs(qt.includedirs())
            libdirs(qt.libdirs())
            links(qt.links())
            buildoptions(qt.buildoptions())
            linkoptions(qt.linkoptions())
            configuration 'release'
                targetdir 'build/release'
                defines {'NDEBUG'}
                flags {'OptimizeSpeed'}
            configuration 'debug'
                targetdir 'build/debug'
                defines {'DEBUG'}
                flags {'Symbols'}
            configuration 'linux'
                links {'pthread'}
                buildoptions {'-std=c++11'}
                linkoptions {'-std=c++11'}
            configuration 'macosx'
                buildoptions {'-std=c++11'}
                linkoptions {'-std=c++11'}
            configuration 'windows'
                files {'.clang-format'}
    end
