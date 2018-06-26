![chameleon](banner.png "The Chameleon banner")

Chameleon provides Qt components for event streams display.

# install

## clone

Within a Git repository, run the commands:

```sh
mkdir -p third_party
cd third_party
git submodule add https://github.com/neuromorphic-paris/chameleon.git
git submodule update --init --recursive
```

## dependencies

An application using Chameleon must link to several Qt libraries, as described in the file [qt.lua](blob/master/qt.lua). The page [use Qt in a premake project](https://github.com/neuromorphic-paris/chameleon/wiki/use-Qt-in-a-premake-project) provides documentation for this file.

### Debian / Ubuntu

Open a terminal and run:
```sh
sudo apt install qtbase5-dev qtdeclarative5-dev qml-module-qtquick-controls qml-module-qtquick-controls2 # GUI toolkit
```

### macOS

Open a terminal and run:
```sh
brew install qt # GUI toolkit
```
If the command is not found, you need to install Homebrew first with the command:
```sh
ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
```

### Windows

Download and install [Qt](https://www.qt.io/download). Select the latest 32-bits version when asked. You may want to restrict the installation to your platform, as the default setup will install pre-compiled versions for other platforms as well, and take up a lot of space. After the installation, open a command prompt as administrator and run:
```batch
mklink /D c:\Qt\opt c:\Qt\5.11.1\msvc2015
```
You may need to change `5.11.1` and `msvc2015` to match your Qt version and platform.

Finally, add `c:\Qt\opt\bin` to your path and reboot your computer.

# user guides and documentation

Code documentation is held in the [wiki](https://github.com/neuromorphic-paris/chameleon/wiki).

# contribute

## development dependencies

### Debian / Ubuntu

Open a terminal and run:
```sh
sudo apt install premake4 # cross-platform build configuration
sudo apt install clang-format # formatting tool
```

### macOS

Open a terminal and run:
```sh
brew install premake # cross-platform build configuration
brew install clang-format # formatting tool
```

### Windows

Download and install:
- [Visual Studio Community](https://visualstudio.microsoft.com/vs/community/). Select at least __Desktop development with C++__ when asked.
- [git](https://git-scm.com)
- [premake 4.x](https://premake.github.io/download.html). In order to use it from the command line, the *premake4.exe* executable must be copied to a directory in your path. After downloading and decompressing *premake-4.4-beta5-windows.zip*, run from the command line:
```sh
copy "%userprofile%\Downloads\premake-4.4-beta5-windows\premake4.exe" "%userprofile%\AppData\Local\Microsoft\WindowsApps"
```

## test

To test the library, run from the *chameleon* directory:
```sh
premake4 gmake
cd build
make
cd release
```

__Windows__ users must run `premake4 vs2010` instead, and open the generated solution with Visual Studio.

You can then run sequentially the executables located in the *release* directory.

After changing the code, format the source files by running from the *chameleon* directory:
```sh
for file in source/*.hpp; do clang-format -i $file; done;
for file in test/*.cpp; do clang-format -i $file; done;
```

__Windows__ users must run *Edit* > *Advanced* > *Format Document* from the Visual Studio menu instead.

# license

See the [LICENSE](LICENSE.txt) file for license rights and limitations (GNU GPLv3).
