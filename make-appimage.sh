#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
repo=$PWD
app=mx-live-usb-maker
output=$repo/build-appimage
tools=$output/tools

offline=false
if [[ $# == 1 && $1 == --offline ]]; then
    offline=true
elif [[ $# != 0 ]]; then
    echo "Usage: $0 [--offline]" >&2
    exit 2
fi

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
        if "$offline"; then
            echo "Missing cached file for offline build: $name" >&2
            exit 1
        fi
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

mxrepo=https://mxrepo.com/mx/repo
keyring=mx25-archive-keyring_2025.03_all.deb
fetch "$keyring" "$mxrepo/pool/main/m/mx25-archive-keyring/$keyring" \
    c490bc3fa24b8fe1c3850c0f5175b1e7f595c379369c08890db5044e26385bee
dpkg-deb -x "$tools/$keyring" "$work/keyring"

# Give APT its own sources, keys and package lists, including on plain Debian.
aptEtc=$work/keyring/etc/apt
# APT's _apt user needs to reach the public key when CI runs as root.
chmod 711 "$work"
mkdir -p "$aptEtc/apt.conf.d" "$aptEtc/preferences.d" "$aptEtc/sources.list.d" "$tools/mx25-lists/partial"
echo "deb [arch=amd64] $mxrepo trixie main" > "$aptEtc/sources.list"
cat > "$work/apt.conf" <<EOF
Dir::Etc "$aptEtc";
Dir::State::lists "$tools/mx25-lists";
Dir::State::status "/dev/null";
Dir::Cache "$work";
Acquire::Languages "none";
EOF
if ! "$offline"; then
    APT_CONFIG="$work/apt.conf" apt-get --error-on=any update
fi

packages=$work/packages
backend=$work/backend
for package in live-usb-maker cli-shell-utils; do
    record=$(APT_CONFIG="$work/apt.conf" apt-cache --no-all-versions show "$package")
    path=$(sed -n 's/^Filename: //p' <<< "$record")
    digest=$(sed -n 's/^SHA256: //p' <<< "$record")
    fetch "${path##*/}" "$mxrepo/$path" "$digest"
    dpkg-deb -x "$tools/${path##*/}" "$packages"
done
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
