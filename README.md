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

## test

To test the library, run from the *chameleon* directory:
```sh
premake4 gmake
cd build
make
cd release
```
You can then run sequentially the executables located in the *release* directory.

After changing the code, format the source files by running from the *chameleon* directory:
```sh
for file in source/*.hpp; do clang-format -i $file; done;
for file in test/*.cpp; do clang-format -i $file; done;
```

# license

See the [LICENSE](LICENSE.txt) file for license rights and limitations (GNU GPLv3).
