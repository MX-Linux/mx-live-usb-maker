# Live USB Maker

[![latest packaged version(s)](https://repology.org/badge/latest-versions/mx-live-usb-maker.svg)](https://repology.org/project/mx-live-usb-maker/versions)
[![build result](https://build.opensuse.org/projects/home:mx-packaging/packages/mx-live-usb-maker/badge.svg?type=default)](https://software.opensuse.org//download.html?project=home%3Amx-packaging&package=mx-live-usb-maker)

Program for creating a live-usb from an iso-file, another live-usb, a live-cd/dvd, or a running live system.

![image](https://github.com/MX-Linux/mx-live-usb-maker/assets/418436/e2c4c12a-da85-41fe-99d5-e61f0253776c)

## Build an AppImage

GitHub Actions builds the x86-64 AppImage from each non-Arch release tag and attaches it to that tag’s GitHub release. Download it from the release’s **Assets** section and make it executable with `chmod +x`.

On x86-64 MX Linux or Debian, install the build dependencies once.

```sh
sudo apt install build-essential cmake ninja-build dpkg-dev curl file qt6-base-dev \
    qt6-tools-dev qt6-tools-dev-tools qt6-svg-plugins qt6-translations-l10n qt6-wayland
```

Run the builder as your normal user.

```sh
./make-appimage.sh
```

The script writes `build-appimage/mx-live-usb-maker-<version>-x86_64.AppImage` using the version from `debian/changelog`. It downloads the packaging tools and MX backend packages on the first build, checks their SHA-256 hashes, and caches them in `build-appimage/tools/` for offline rebuilds. For distribution, build on the oldest supported system so the required glibc and libstdc++ versions are available on your targets.

Run the AppImage as your desktop user. It includes the MX/antiX `live-usb-maker` backend, its `cli-shell-utils` library and helpers, Qt, translations, and offline help. The host supplies Bash, `pkexec` with a running polkit authentication agent, and the disk tools used by the selected mode. These include `xorriso` for ISO detection, `parted` and `fdisk` for partitioning, filesystem utilities such as `mkfs.vfat` and `mkfs.ext4`, and `extlinux` and the Syslinux boot files for BIOS boot. Encryption also uses `cryptsetup` and `dmsetup`. If FUSE is unavailable, launch the image with `--appimage-extract-and-run`.
