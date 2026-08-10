# FlexClock

Replaces the iOS lock screen clock with images, and lets you install your own digit packs.

Built against the `CoverSheetKit` lock screen, for iOS 18.

![FlexClock on the lock screen](screenshot.png)

## Features

- Animated (GIF) or static digits
- Installable digit packs, imported and removed from Settings
- Optional switch to still frames while Low Power Mode is on
- Adjustable digit size, spacing and vertical offset
- 12-hour / 24-hour / system time format
- Optional date hiding
- Colour tinting that keeps the artwork's shading, the way iOS tints app icons
- Settings apply live, no respring needed
- Falls back to the stock clock if the selected pack is missing or broken

## Requirements

- iOS 18, rootless jailbreak (developed on 18.5, Dopamine 3.0.4)
- `preferenceloader`
- ElleKit / Substrate

## Installation

Add the repository in Sileo:

```
https://tr3tol.github.io/repo/
```

Or install a release `.deb` by hand:

```sh
dpkg -i com.tr3tol.flexclock_1.0.1_iphoneos-arm64.deb
killall -9 SpringBoard
```

Settings live under **Settings → FlexClock**.

## Digit packs

Packs are plain folders. The bundled ones ship with the package:

```
/var/jb/Library/Application Support/FlexClock/Packs/
```

Imported ones live outside it, so they survive updates and can be managed from
Settings:

```
/var/mobile/Library/FlexClock/Packs/
```

A user pack shadows a bundled pack of the same name, so the included artwork can
be replaced without touching the installed package.

A pack is a folder whose name ends in `.flexpack`:

```
MyPack.flexpack/
├── Info.plist
├── animated/          # optional
│   ├── 0.gif … 9.gif
│   └── colon.gif
└── static/            # optional
    ├── 0.png … 9.png
    └── colon.png
```

- At least one of `animated/` or `static/` must exist. If the folder for the
  selected animation style is missing, the other one is used.
- `.gif` and `.png` are both accepted in either folder. Animated GIFs play at
  their own frame timing.
- Digits are laid out by their aspect ratio, so frames do not have to be square
  or share a common width.
- `Info.plist` is optional; without it the folder name is used as the title.

```xml
<dict>
    <key>Name</key>    <string>My Pack</string>
    <key>Version</key> <string>1.0</string>
</dict>
```

**Settings → FlexClock → Manage packs** lists what is installed, imports a pack
folder and removes the imported ones. A folder is only accepted if it actually
contains digits; if the pack sits one level below what you picked, it is found
there. Copying a folder into the directory above works just as well.

Packs can also be shipped as `.deb` packages that install into the bundled
directory.

## Building

Requires [Theos](https://theos.dev) with a rootless setup:

```sh
make package THEOS_PACKAGE_SCHEME=rootless
```

## Credits

- Idea — [yandevelop](https://github.com/yandevelop), author of the original
  AniTime tweak. FlexClock is an independent reimplementation for modern iOS.
- Development — [tr3tol](https://github.com/tr3tol)

## License

Source code is released under the GNU General Public License v3.0, see
[LICENSE](LICENSE). Bundled digit artwork is not covered by it and remains the
property of its respective authors.
