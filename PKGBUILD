# forkfarm darktable PKGBUILD - builds from r4debox/darktable fork
# based on arch official PKGBUILD, adapted for git source

pkgname=darktable
epoch=2
pkgver=5.6.1
pkgrel=1
pkgdesc='Utility to organize and develop raw images (r4debox fork)'
arch=(x86_64)
url='https://github.com/r4debox/darktable'
license=(GPL-3.0-or-later)
depends=(colord-gtk
         exiv2
         flickcurl
         gmic
         graphicsmagick
         iso-codes
         jasper
         lensfun
         libavif
         libgphoto2
         libjpeg-turbo
         libjxl
         libsecret
         libxml2
         lua54
         openexr
         openjpeg2
         openmp
         osm-gps-map
         potrace
         pugixml
         zlib)
optdepends=('dcraw: base curve script'
            'ghostscript: noise profile script'
            'gnuplot: noise profile script'
            'imagemagick: base curve and noise profile scripts'
            'opencl-driver: hardware accelerated image processing'
            'perl-image-exiftool: base curve script'
            'portmidi: game and midi controller input devices')
makedepends=(clang
             cmake
             desktop-file-utils
             intltool
             libwebp
             llvm
             portmidi
             python-jsonschema)
source=("$pkgname::git+https://github.com/r4debox/darktable.git")
sha256sums=('SKIP')

pkgver() {
    cd "$pkgname"
    git describe --long --tags 2>/dev/null | sed 's/^release-//;s/\([^-]*-g\)/r\1/;s/-/./g' || echo "$pkgver"
}

build() {
    local cmake_flags=(
        PROJECT_VERSION="$pkgver"
        CMAKE_INSTALL_PREFIX=/usr
        CMAKE_INSTALL_LIBEXECDIR=/usr/lib
        CMAKE_BUILD_TYPE=Release
        BINARY_PACKAGE_BUILD=1
        USE_AI=On
        USE_COLORD=On
        USE_LIBSECRET=On
        USE_LUA=On
        BUILD_CURVE_TOOLS=On
        BUILD_NOISE_TOOLS=On
        BUILD_USERMANUAL=Off
        RAWSPEED_ENABLE_LTO=On
    )
    cmake -B build -S "$pkgname" ${cmake_flags[@]/#/-D }
    make -C build
}

package() {
    make -C build DESTDIR="$pkgdir" install
    ln -s darktable/libdarktable.so "$pkgdir/usr/lib/"
}
