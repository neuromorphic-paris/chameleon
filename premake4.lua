solution 'chameleon'
    configurations {'Release', 'Debug'}
    location 'build'

    newaction {
        trigger = "install",
        description = "Install the library",
        execute = function ()
            os.execute('rm -rf /usr/local/include/chameleon')
            os.mkdir('/usr/local/include/chameleon')
            for index, filename in pairs(os.matchfiles('source/*.hpp')) do
                os.copyfile(filename, path.join('/usr/local/include/chameleon', path.getname(filename)))
            end

            print(string.char(27) .. '[32mChameleon library installed.' .. string.char(27) .. '[0m')
            os.exit()
        end
    }

    newaction {
        trigger = 'uninstall',
        description = 'Remove all the files installed during build processes',
        execute = function ()
            os.execute('rm -rf /usr/local/include/chameleon')
            print(string.char(27) .. '[32mChameleon library uninstalled.' .. string.char(27) .. '[0m')
            os.exit()
        end
    }

    project 'chameleonTest'
        -- General settings
        kind 'ConsoleApp'
        language 'C++'
