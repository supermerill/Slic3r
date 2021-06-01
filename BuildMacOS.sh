#!/bin/bash

export ROOT=`pwd`
export NCORES=`sysctl -n hw.ncpu`

while getopts ":ih" opt; do
  case ${opt} in
    i )
        export BUILD_IMAGE="1"
        ;;
    h ) echo "Usage: ./BuildMacOS.sh [-i]"
        echo "   -i: Generate DMG image (optional)"
        exit 0
        ;;
  esac
done

# mkdir build
if [ ! -d "build" ]
then
    mkdir build
fi

echo -n "[1/9] Updating submodules..."
{
    # update submodule profiles
    pushd resources/profiles
    git submodule update --init
    popd
} > $ROOT/build/MacOS_Build.log # Capture all command output
echo "done"

echo -n "[2/9] Changing date in version..."
{
    # change date in version
    sed "s/+UNKNOWN/_$(date '+%F')/" version.inc > version.date.inc
    mv version.date.inc version.inc
} &> $ROOT/build/MacOS_Build.log # Capture all command output
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
} &> $ROOT/build/MacOS_Build.log # Capture all command output
echo "done"

echo -n "[4/9] Building dependencies..."
{
    # make deps
    make -j$NCORES
} &> $ROOT/build/MacOS_Build.log # Capture all command output
echo "done"

echo -n "[5/9] Renaming wxscintilla library..."
{
    # rename wxscintilla
    pushd destdir/usr/local/lib
    cp libwxscintilla-3.1.a libwx_osx_cocoau_scintilla-3.1.a
    popd
} &> $ROOT/build/MacOS_Build.log # Capture all command output
echo "done"

echo -n "[6/9] Cleaning dependencies..."
{
    # clean deps
    rm -rf dep_*
    popd
} &> $ROOT/build/MacOS_Build.log # Capture all command output
echo "done"

echo -n "[7/9] Configuring SuperSlicer..."
{
    # cmake
    pushd build
    cmake .. -DCMAKE_PREFIX_PATH="$PWD/../deps/build/destdir/usr/local" -DCMAKE_OSX_DEPLOYMENT_TARGET="10.13" -DSLIC3R_STATIC=1
} &> $ROOT/build/MacOS_Build.log # Capture all command output
echo "done"

echo -n "[8/9] Building SuperSlicer..."
{
    # make SuperSlicer
    make -j$NCORES Slic3r

    # make .mo
    make gettext_po_to_mo
} &> $ROOT/build/MacOS_Build.log # Capture all command output
echo "done"

echo -n "[9/9] Generating MacOS app..."
{
    # update Info.plist
    pushd src
    sed "s/+UNKNOWN/_$(date '+%F')/" Info.plist >Info.date.plist
    popd

    # create directory and copy into it
    if [ -d "pack" ]
    then
        rm -rf pack/*
    fi
    mkdir pack
    mkdir pack/SuperSlicer
    mkdir pack/SuperSlicer/SuperSlicer.app
    mkdir pack/SuperSlicer/SuperSlicer.app/Contents
    mkdir pack/SuperSlicer/SuperSlicer.app/Contents/_CodeSignature
    mkdir pack/SuperSlicer/SuperSlicer.app/Contents/Frameworks
    mkdir pack/SuperSlicer/SuperSlicer.app/Contents/MacOS

    # copy Resources
    cp -Rf ../resources pack/SuperSlicer/SuperSlicer.app/Contents/Resources
    cp pack/SuperSlicer/SuperSlicer.app/Contents/Resources/icons/SuperSlicer.icns pack/SuperSlicer/SuperSlicer.app/Contents/resources/SuperSlicer.icns
    cp src/Info.date.plist pack/SuperSlicer/SuperSlicer.app/Contents/Info.plist
    echo -n -e 'APPL????\x0a' > PkgInfo
    cp PkgInfo pack/SuperSlicer/SuperSlicer.app/Contents/PkgInfo

    # copy bin and do not let it lower case
    cp -f src/superslicer pack/SuperSlicer/SuperSlicer.app/Contents/MacOS/SuperSlicer
    chmod u+x pack/SuperSlicer/SuperSlicer.app/Contents/MacOS/SuperSlicer
} &> $ROOT/build/MacOS_Build.log # Capture all command output
echo "done"

if [[ -n "$BUILD_IMAGE" ]]
then
echo -n "Creating DMG Image for distribution..."
{
    tar -cvf SuperSlicer.tar pack/SuperSlicer

    # create dmg
    hdiutil create -ov -fs HFS+ -volname "SuperSlicer" -srcfolder "pack/SuperSlicer" temp.dmg
    hdiutil convert temp.dmg -format UDZO -o SuperSlicer.dmg
    popd
} &> $ROOT/build/MacOS_Build.log # Capture all command output
echo "done"
fi
