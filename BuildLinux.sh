#!/bin/bash

export ROOT=`pwd`
export NCORES=`sysctl -n hw.ncpu`

while getopts ":ih" opt; do
  case ${opt} in
    i )
        export BUILD_IMAGE="1"
        ;;
    h ) echo "Usage: ./BuildLinux.sh [-i][-u]"
        echo "   -i: Generate appimage (optional)"
        echo "   -u: only update clock & dependency packets (optional and need sudo)"
        exit 0
        ;;
  esac
done

# mkdir build
if [ ! -d "build" ]
then
    mkdir build
fi


if [[ -n "$BUILD_IMAGE" ]]
then
    echo -n "Updating linux ..."
    {
        hwclock -s
        apt update
        apt install libgtk2.0-dev libglew-dev libudev-dev libdbus-1-dev
    } > $ROOT/build/Build.log # Capture all command output
    echo "done"
    exit 0
fi

echo -n "[1/9] Updating submodules..."
{
    # update submodule profiles
    pushd resources/profiles
    git submodule update --init
    popd
} > $ROOT/build/Build.log # Capture all command output


echo -n "[2/9] Changing date in version..."
{
    # change date in version
    sed "s/+UNKNOWN/_$(date '+%F')/" version.inc > version.date.inc
    mv version.date.inc version.inc
} &> $ROOT/build/Build.log # Capture all command output
echo "done"

# mkdir in deps
if [ ! -d "deps/build" ]
then
    mkdir deps/build
fi

echo -n "[3/9] Configuring dependencies..."
{
    # cmake deps
    pushd deps/build
    cmake .. -DCMAKE_OSX_DEPLOYMENT_TARGET="10.13"
} &> $ROOT/build/Build.log # Capture all command output
echo "done"

echo -n "[4/9] Building dependencies..."
{
    # make deps
    make -j$NCORES
} &> $ROOT/build/Build.log # Capture all command output
echo "done"

echo -n "[5/9] Renaming wxscintilla library..."
{
    # rename wxscintilla
    pushd destdir/usr/local/lib
    cp libwxscintilla-3.1.a libwx_gtk2u_scintilla-3.1.a
    popd
} &> $ROOT/build/Build.log # Capture all command output
echo "done"

echo -n "[6/9] Cleaning dependencies..."
{
    # clean deps
    rm -rf dep_*
    popd
} &> $ROOT/build/Build.log # Capture all command output
echo "done"

echo -n "[7/9] Configuring Slic3r..."
{
    # cmake
    pushd build
    cmake .. -DCMAKE_PREFIX_PATH="$PWD/../deps/build/destdir/usr/local" -DCMAKE_OSX_DEPLOYMENT_TARGET="10.13" -DSLIC3R_STATIC=1
} &> $ROOT/build/Build.log # Capture all command output
echo "done"

echo -n "[8/9] Building Slic3r..."
{
    # make Slic3r
    make -j$NCORES Slic3r

    # make .mo
    make gettext_po_to_mo
} &> $ROOT/build/Build.log # Capture all command output
echo "done"

echo -n "[9/9] Generating Linux app..."
{
    if [[ -n "$BUILD_IMAGE" ]]
    then
        $ROOT/build/BuildLinuxImage.sh -i
    else
        $ROOT/build/BuildLinuxImage.sh
    fi
} &> $ROOT/build/Build.log # Capture all command output
