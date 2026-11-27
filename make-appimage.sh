#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
repo=$PWD
app=mx-live-usb-maker
output=$repo/build-appimage
tools=$output/tools

if [[ $(uname -sm) != 'Linux x86_64' ]]; then
    echo "This script builds an x86-64 AppImage on x86-64 Linux." >&2
    exit 1
fi

mkdir -p "$tools"
work=$(mktemp -d "$output/staging.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT

# Keep verified tools for subsequent offline builds.
fetch() {
    local name=$1 url=$2 digest=$3
    if [[ ! -f $tools/$name ]]; then
        curl --fail --location --retry 3 --output "$work/$name" "$url"
        (cd "$work" && echo "$digest  $name" | sha256sum --check)
        mv -f -- "$work/$name" "$tools/$name"
    else
        (cd "$tools" && echo "$digest  $name" | sha256sum --check)
    fi
}

deploy=linuxdeploy-x86_64.AppImage
plugin=linuxdeploy-plugin-qt-x86_64.AppImage
fetch "$deploy" "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/$deploy" \
    c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
fetch "$plugin" "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/$plugin" \
    15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
fetch runtime-x86_64 https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
    2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
chmod 755 "$tools/$deploy" "$tools/$plugin" "$tools/runtime-x86_64"

pool=https://mxrepo.com/mx/repo/pool/main
lum=live-usb-maker_2.41.29_all.deb
utils=cli-shell-utils_0.4.2+10mx25_all.deb
fetch "$lum" "$pool/l/live-usb-maker/$lum" \
    21f5ac5b0c2bee94ce527478c590c1a94a2771438af2928e4e863ca6d5142e9e
fetch "$utils" "$pool/c/cli-shell-utils/$utils" \
    8d06ea0c94b4996706046ffdf44a134ad943c131d6d02d49f7fc0f7463f7d077

packages=$work/packages
backend=$work/backend
dpkg-deb -x "$tools/$lum" "$packages"
dpkg-deb -x "$tools/$utils" "$packages"
install -Dm755 "$packages/usr/bin/live-usb-maker" "$backend/bin/live-usb-maker"
cp -R "$packages/usr/lib/cli-shell-utils" "$backend/"
cp -R "$packages/usr/share/locale" "$backend/cli-shell-utils/"
tar -czf "$work/backend.tar.gz" -C "$backend" bin cli-shell-utils

build=$work/build
appdir=$work/AppDir
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DAPPIMAGE_BUILD=ON -DAPPIMAGE_BACKEND_ARCHIVE="$work/backend.tar.gz"
cmake --build "$build" --parallel
DESTDIR="$appdir" cmake --install "$build"

share=$appdir/usr/share
mkdir -p "$share/doc"
cp -R "$packages/usr/share/doc/"{live-usb-maker,cli-shell-utils} "$share/doc/"
if ! "$appdir/usr/libexec/$app/helper" exec live-usb-maker --help > "$work/backend-help" ||
    ! grep -q -- '--data-first' "$work/backend-help"; then
    echo "The bundled live-usb-maker backend failed its help check." >&2
    exit 1
fi
install -Dm644 -t "$share/$app/locale" "$build"/"$app"_*.qm
install -Dm644 -t "$share/doc/$app" help/* authors.txt LICENSE
gzip -nc debian/changelog > "$share/doc/$app/changelog.gz"

version=$(dpkg-parsechangelog -SVersion)
filename=$app-$version-x86_64.AppImage
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$tools:$PATH"
export QMAKE
QMAKE=$(command -v qmake6)
qtPlugins=$("$QMAKE" -query QT_INSTALL_PLUGINS)
plugins=$appdir/usr/plugins
install -Dm644 -t "$plugins/platforms" "$qtPlugins"/platforms/libqwayland*.so
deployDeps=(--deploy-deps-only "$appdir/usr/libexec/$app/helper" --deploy-deps-only "$plugins/platforms")
for group in wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
    install -Dm644 -t "$plugins/$group" "$qtPlugins/$group/"*.so
    deployDeps+=(--deploy-deps-only "$plugins/$group")
done
export LDAI_RUNTIME_FILE="$tools/runtime-x86_64"
export LDAI_OUTPUT="$work/$filename"
"$tools/$deploy" --appdir "$appdir" "${deployDeps[@]}" \
    --desktop-file "$app.desktop" --icon-file "$app.png" --icon-file "$app.svg" --plugin qt --output appimage

if [[ $("$work/$filename" --version) != "$app $version" ]]; then
    echo "The packaged application failed its version check." >&2
    exit 1
fi
mv -f -- "$work/$filename" "$output/$filename"
echo "Built $output/$filename"
