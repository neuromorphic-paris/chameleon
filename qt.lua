local qt = {}

local os_to_default_configuration = {
    bsd = {

    },
    linux = {
        moc = '/usr/lib/x86_64-linux-gnu/qt5/bin/moc',
        moc_includedirs = {
            '/usr/include/qt5/QtQml',
            '/usr/include/x86_64-linux-gnu/qt5/QtQml'},
        lib = '/usr/lib/x86_64-linux-gnu',
        includedirs = {
            '/usr/include/qt5/',
            '/usr/include/qt5/QtQml',
            '/usr/include/x86_64-linux-gnu/qt5',
            '/usr/include/x86_64-linux-gnu/qt5/QtQml'},
        libdirs = {'/usr/lib/x86_64-linux-gnu'},
        links = {'Qt5Core', 'Qt5Gui', 'Qt5Qml', 'Qt5Quick'},
        buildoptions = {'-fPIC'},
        linkoptions = {},
    },
    macosx = {
        moc = '/usr/local/opt/qt/bin/moc',
        moc_includedirs = {'/usr/local/opt/qt/include/QtQml'},
        lib = '/usr/local/opt/qt/lib',
        includedirs = {
            '/usr/local/opt/qt/include',
            '/usr/local/opt/qt/include/QtQml'},
        libdirs = {},
        links = {},
        buildoptions = {'-Wno-comma'},
        linkoptions = {
            '-F /usr/local/opt/qt/lib',
            '-framework QtCore',
            '-framework QtGui',
            '-framework QtQml',
            '-framework QtQuick',
            '-framework QtQuickControls2'},
    },
    solaris = {

    },
    windows = {

    },
}

-- generate_configuration merges the given configuration map with the default one.
-- The current os configuration is extracted with premake's os.get().
function generate_configuration(os_to_configuration)
    if os_to_configuration == nil then
        os_to_configuration = {}
    end
    setmetatable(os_to_configuration, {__index = os_to_default_configuration})
    local configuration = os_to_configuration[os.get()]
    setmetatable(configuration, {__index = os_to_default_configuration[os.get()]})
    return configuration
end

-- moc calls Qt's moc on each file, and writes the result to target_directory.
-- os_to_configuration can be used to override os-specific configurations.
-- Only parameters different from the default need to be specified.
function qt.moc(files, target_directory, os_to_configuration)
    local configuration = generate_configuration(os_to_configuration)
    os.mkdir(target_directory)
    local generated_files = {}
    local moc_includes = {}
    for index, includedir in ipairs(configuration.moc_includedirs) do
        moc_includes[index] = '-I\'' .. includedir .. '\''
    end
    for index, file in pairs(files) do
        local target_file = target_directory .. '/' .. path.getname(file) .. '.cpp'
        if os.execute(
            configuration.moc
            .. ' ' .. table.concat(moc_includes, ' ')
            .. ' -o \'' .. target_file .. '\''
            .. ' \''.. file .. '\''
        ) ~= 0 then
            print('Qt error: running moc on ' .. file .. ' failed')
            os.exit(1)
        end
        generated_files[index] = target_file
    end
    return generated_files
end

-- includedirs returns a list to be passed to premake's includedirs function.
-- os_to_configuration can be used to override os-specific configurations.
-- Only parameters different from the default need to be specified.
function qt.includedirs(os_to_configuration)
    local configuration = generate_configuration(os_to_configuration)
    return configuration.includedirs
end

-- libdirs returns a list to be passed to premake's libdirs function.
-- os_to_configuration can be used to override os-specific configurations.
-- Only parameters different from the default need to be specified.
function qt.libdirs(os_to_configuration)
    local configuration = generate_configuration(os_to_configuration)
    return configuration.libdirs
end

-- links returns a list to be passed to premake's links function.
-- os_to_configuration can be used to override os-specific configurations.
-- Only parameters different from the default need to be specified.
function qt.links(os_to_configuration)
    local configuration = generate_configuration(os_to_configuration)
    return configuration.links
end

-- buildoptions returns a list to be passed to premake's buildoptions function.
-- os_to_configuration can be used to override os-specific configurations.
-- Only parameters different from the default need to be specified.
function qt.buildoptions(os_to_configuration)
    local configuration = generate_configuration(os_to_configuration)
    return configuration.buildoptions
end


-- linkoptions returns a list to be passed to premake's linkoptions function.
-- os_to_configuration can be used to override os-specific configurations.
-- Only parameters different from the default need to be specified.
function qt.linkoptions(os_to_configuration)
    local configuration = generate_configuration(os_to_configuration)
    return configuration.linkoptions
end

return qt
