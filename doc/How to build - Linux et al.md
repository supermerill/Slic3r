
# Building SuperSlicer on UNIX/Linux


SuperSlicer uses the CMake build system and requires several dependencies.
The dependencies can be listed in `deps/deps-linux.cmake` and `deps/deps-unix-common.cmake`, although they don't necessarily need to be as recent
as the versions listed - generally versions available on conservative Linux distros such as Debian stable or CentOS should suffice.

Perl is not required anymore.

In a typical situation, one would open a command line, go to the SuperSlicer sources, create a directory called `build` or similar,
`cd` into it and call:

    cmake ..
    make -jN

where `N` is the number of CPU cores available.

Additional CMake flags may be applicable as explained below.

### How to get the source code

You have to gitclone  the repository
```
git clone https://github.com/supermerill/SuperSlicer.git
```

and then you have to clone the profiles submodules

```
cd resources/profiles
git submodule update
```

## How to build, the easy way

You can follow the [script](https://github.com/supermerill/SuperSlicer/blob/master/.github/workflows/ccpp_ubuntu.yml) the build server use to create the ubuntu release.

You have to execute each command at the right of the 'run: ' tags, in the directory that is at the right of the previous 'working-directory:' tag.

You can stop after the `make slic3r` as the rest of the commands are for building the launch script and the appimage.

## How to build, other ways

### Dependency resolution

By default SuperSlicer looks for dependencies the default way CMake looks for them, i.e. in default system locations.
On Linux this will typically make SuperSlicer depend on dynamically loaded libraries from the system, however, SuperSlicer can be told
to specifically look for static libraries with the `SLIC3R_STATIC` flag passed to cmake:

    cmake .. -DSLIC3R_STATIC=1

Additionally, SuperSlicer can be built in a static manner mostly independent of the system libraries with a dependencies bundle
created using CMake script in the `deps` directory (these are not interconnected with the rest of the CMake scripts).

Note: We say _mostly independent_ because it's still expected the system will provide some transitive dependencies, such as GTK for wxWidgets.

To do this, go to the `deps` directory, create a `build` subdirectory (or the like) and use:

    cmake .. -DDESTDIR=<target destdir>

where the target destdir is a directory of your choosing where the dependencies will be installed.
You can also omit the `DESTDIR` option to use the default, in that case the `destdir` will be created inside the `build` directory where `cmake` is run.

To pass the destdir path to the top-level SuperSlicer CMake script, use the `CMAKE_PREFIX_PATH` option along with turning on `SLIC3R_STATIC`:

    cmake .. -DSLIC3R_STATIC=1 -DCMAKE_PREFIX_PATH=<path to destdir>/usr/local

Note that `/usr/local` needs to be appended to the destdir path and also the prefix path should be absolute.

**Warning**: Once the dependency bundle is installed in a destdir, the destdir cannot be moved elsewhere.
This is because wxWidgets hardcode the installation path.

### wxWidgets version

SuperSlicer need at least wxWidgets 3.1

### Build variant

By default SuperSlicer builds the release variant.
To create a debug build, use the following CMake flag:

    -DCMAKE_BUILD_TYPE=Debug

### Enabling address sanitizer

If you're using GCC/Clang compiler, it is possible to build SuperSlicer with the built-in address sanitizer enabled to help detect memory-corruption issues.
To enable it, simply use the following CMake flag:

    -DSLIC3R_ASAN=1

This requires GCC>4.8 or Clang>3.1.

### Installation

At runtime, SuperSlicer needs a way to access its resource files. By default, it looks for a `resources` directory relative to its binary.

If you instead want SuperSlicer installed in a structure according to the File System Hierarchy Standard, use the `SLIC3R_FHS` flag

    cmake .. -DSLIC3R_FHS=1

This will make SuperSlicer look for a fixed-location `share/slic3r-prusa3d` directory instead (note that the location becomes hardcoded).

You can then use the `make install` target to install SuperSlicer.


## Raspberry pi

Some hints to be able to build on raspberry pi:

I think that the most important thing is preparing Raspberry Pi. I'm using overclocked to 2GHz RPi 4B 2GB with active cooling(it starts when the temperature is too high). 2GB of RAM is not enough and I turned swap on(+2GB), but i use SSD instead of memory card so it's not that bad. Swap is only needed during compilation, built-in memory is enough to run the GUI.

first, you have to install all the dependencies  (you can also try to build them from the deps directory):
`sudo apt-get install -y git cmake libboost-dev libboost-regex-dev libboost-filesystem-dev libboost-thread-dev libboost-log-dev libboost-locale-dev libcurl4-openssl-dev libwxgtk3.0-dev build-essential pkg-config libtbb-dev zlib1g-dev libcereal-dev libeigen3-dev libnlopt-cxx-dev libudev-dev libopenvdb-dev libboost-iostreams-dev libnlopt-dev libdbus-1-dev`

Running cmake will give information about the remaining missing dependencies. (to be run in a newly created build directory)
`cmake .. -DCMAKE_BUILD_TYPE=Release -DSLIC3R_BUILD_TESTS=OFF -DSLIC3R_GTK = 2`
Note that you can also use different parameters, like
`cmake .. -DSLIC3R_STATIC=0 -DCMAKE_BUILD_TYPE=Debug -DSLIC3R_FHS=1 -DSLIC3R_GTK=2 -DSLIC3R_PCH=0 -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib`

Then, when cmake end without errors, you can compile with the make command.

I think it is a good idea to run this command in screen, because it takes a long time to complete and the connection may time out. screen will remain running in the background.

Example:
```
screen -S superclicer
#new terminal will open
 make -j4
 ```

and after disconnect:
`screen -x`

Finally, it remains to run `sudo make install`

May also be useful to run
`sudo systemctl set-environment LC_ALL=en_US.UTF-8`

I'm sure there is some steps missing, please open an issue to let us know or open a pull request by editing this document.







