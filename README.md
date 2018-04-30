![chameleon](banner.png "The Chameleon banner")

Chameleon provides Qt components for event streams display.

# Install

Within a Git repository, run the commands:

```sh
mkdir -p third_party
cd third_party
git submodule add https://github.com/neuromorphic-paris/chameleon.git
```

# User guides and documentation

Code documentation is held in the [wiki](https://github.com/neuromorphic-paris/chameleon/wiki).

# Contribute

## Development dependencies

Chameleon relies on [Premake 4.x](https://github.com/premake/premake-4.x) (x ≥ 3) to generate build configurations. Follow these steps to install it:
  - __Debian / Ubuntu__: Open a terminal and execute the command `sudo apt-get install premake4`.
  - __OS X__: Open a terminal and execute the command `brew install premake`. If the command is not found, you need to install Homebrew first with the command<br />
  `ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"`.

  [Qt 5.x](https://www.qt.io) is required to host the components. Follow these steps to install it:
    - __Debian / Ubuntu__: Open a terminal and execute the command `sudo apt-get install qtbase5-dev qtdeclarative5-dev`.
    - __OS X__: Open a terminal and execute the command `brew install qt`.

[ClangFormat](https://clang.llvm.org/docs/ClangFormat.html) is used to unify coding styles. Follow these steps to install it:
- __Debian / Ubuntu__: Open a terminal and execute the command `sudo apt-get install clang-format`.
- __OS X__: Open a terminal and execute the command `brew install clang-format`.

## Test

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

# License

See the [LICENSE](LICENSE.txt) file for license rights and limitations (GNU GPLv3).
