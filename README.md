# localed

A lightweight implementation of the `org.freedesktop.locale1` D-Bus service
(`localed`) for Slackware and other non-systemd Linux distributions. It
provides system locale and keyboard configuration through the standard
locale1 D-Bus interface, without depending on systemd.

The daemon follows the same general service pattern as
[nater1983/hostnamed](https://github.com/nater1983/hostnamed).

## Features

- Implements the `org.freedesktop.locale1` D-Bus interface.
- Reads and writes Slackware's locale and keyboard configuration files.
- Uses atomic file updates and transactional rollback for multi-file changes.
- Watches configuration files with inotify and emits D-Bus
  `PropertiesChanged` notifications when values change.
- Validates Xorg XKB configuration before accepting or reporting values.
- Uses Polkit for authorization, with a configurable privileged group
  (default: `wheel`).
- Integrates with AccountsService when it is available at runtime.
- Provides optional desktop input-source integration through a build-time
  backend selection.
- Does not require systemd; it uses D-Bus and a logind-compatible session
  manager for desktop session discovery.

The Plasma 6 backend reads the system locale from `[Formats] LANG` in
`~/.config/plasma-localerc`, not the translation preference list in
`[Translations] LANGUAGE`. Missing or invalid `LANG` values are skipped.

## Dependencies

| Library / service | pkg-config name | Minimum version / requirement |
|---|---|---|
| GLib / GIO | `glib-2.0`, `gio-2.0`, `gio-unix-2.0` | 2.56 |
| Polkit | `polkit-gobject-1` | 0.113 |
| D-Bus daemon | — | Runtime requirement |
| logind-compatible service | — | Runtime requirement for desktop session discovery |

The daemon is activated by `dbus-daemon` through D-Bus system-service
activation (`[D-BUS Service]`). There is no systemd dependency.
AccountsService integration is optional at runtime: `localed` monitors its
D-Bus name and synchronizes account language changes when the service is
available. No AccountsService development library is required.

The GNOME input-source backend uses GIO/GSettings and the GNOME session's
D-Bus connection. It does not add a separate helper executable or a separate
build-time GNOME library dependency.

On Debian/Ubuntu:

```sh
apt install libglib2.0-dev libpolkit-gobject-1-dev \
            meson ninja-build pkg-config
```

On Fedora/RHEL:

```sh
dnf install glib2-devel polkit-devel \
            meson ninja-build pkg-config
```

## Building

The default build does not enable a desktop integration backend:

```sh
meson setup build
meson compile -C build
```

To build the GNOME input-source backend:

```sh
meson setup build-gnome -Ddesktop-backend=gnome
meson compile -C build-gnome
```

To build the KDE Plasma 6 input-source backend:

```sh
meson setup build-plasma6 -Ddesktop-backend=plasma6
meson compile -C build-plasma6
```

The backend is selected with the `desktop-backend` Meson option:

| Option | Description |
|---|---|
| `none` | Default. Build localed without a desktop-specific input-source backend. |
| `gnome` | Build the GNOME session and input-source integration. |
| `plasma6` | Build the KDE Plasma 6 session and input-source integration. |

For example, to explicitly select the default configuration:

```sh
meson setup build -Ddesktop-backend=none
```

Automated tests are disabled by default and can be enabled for development
or package validation:

```sh
meson setup build-tests -Dtests=true
meson compile -C build-tests
meson test -C build-tests --print-errorlogs
```

To include the GNOME backend tests:

```sh
meson setup build-gnome-tests -Ddesktop-backend=gnome -Dtests=true
meson compile -C build-gnome-tests
meson test -C build-gnome-tests --print-errorlogs
```

To include the Plasma 6 backend tests:

```sh
meson setup build-plasma6-tests -Ddesktop-backend=plasma6 -Dtests=true
meson compile -C build-plasma6-tests
meson test -C build-plasma6-tests --print-errorlogs
```

The SlackBuild enables the test targets and runs the suite before installing
the package. Test executables are not installed into the resulting package.

### Common Meson options

Install under `/usr` instead of `/usr/local`:

```sh
meson setup build --prefix=/usr
```

Override the D-Bus service directory explicitly:

```sh
meson setup build -Ddbus_system_service_dir=/usr/share/dbus-1/system-services
```

Override locale and keyboard file paths (Slackware defaults shown):

```sh
meson setup build \
    -Drc_keymap=/etc/rc.d/rc.keymap \
    -Dlang_sh=/etc/profile.d/lang.sh \
    -Dprivileged-group=wheel \
    -Dx11_keyboard_conf=/etc/X11/xorg.conf.d/00-keyboard.conf
```

## Installing

```sh
meson install -C build
```

For a system-wide installation, configure with `--prefix=/usr` and install
with the required privileges.

All backend configurations install the same daemon and helper executable
names under `libexecdir` (normally `/usr/libexec`):

```text
/usr/libexec/
├── localed
└── localed-input-sources-helper
```

There is one input-sources helper executable. Desktop-specific helper code
is compiled into that executable when its backend is selected; GNOME does not
install a separately named GNOME helper.

## Desktop input-source integration

Desktop integration is optional and selected at build time with
`-Ddesktop-backend`. The default is `none`, keeping the daemon independent
of desktop-specific APIs.

### GNOME

Build with:

```sh
meson setup build-gnome -Ddesktop-backend=gnome
```

When enabled, the GNOME backend:

- Watches logind for GNOME session changes.
- Identifies the active local GNOME session and its user.
- Locates and connects to that user's GNOME session bus.
- Reads the GNOME input-source settings through GSettings.
- Monitors changes to the GNOME input-source list and active source.
- Synchronizes the complete GNOME XKB layout, variant, and options list to
  the configured Xorg keyboard file.
- Synchronizes the active GNOME source to Slackware's console keymap when a
  matching console keymap mapping is available.

The complete XKB layout list and the active console keymap are deliberately
handled separately. For example, if GNOME has `us` and `es` configured
but `us` is active, the Xorg configuration contains `us,es` while
`rc.keymap` uses `us`. Switching the active GNOME source to `es` changes
the console keymap to `es` without removing either XKB layout from the
Xorg configuration.

The GNOME backend is compiled into `localed`; its session settings reader
is compiled into the shared `localed-input-sources-helper` executable.
The helper executable name and installation path are the same for every
backend configuration.


### KDE Plasma 6

Build with:

```sh
meson setup build-plasma6 -Ddesktop-backend=plasma6
```

The Plasma 6 backend communicates with KWin over the user's session bus:

- Service: `org.kde.keyboard`
- Object path: `/Layouts`
- Interface: `org.kde.KeyboardLayouts`

It calls `getLayoutsList()` to retrieve the ordered `a(sss)` layout list and
`getLayout()` to retrieve the active layout index. KWin's `LayoutNames`
tuple contains `shortName`, `displayName`, and `longName`; the second
field is **not** an XKB variant. The helper reads `VariantList` and
`Options` from the session user's `~/.config/kxkbrc` and aligns variants
with the ordered layout list.

The helper monitors KWin's `layoutChanged` and `layoutListChanged` signals.
Active-layout changes update the Slackware console keymap, while layout-list
changes refresh the complete XKB layout, variant, and options configuration
in the configured Xorg keyboard file. The existing XKB validation and
console-keymap mapping are reused.

As with GNOME, the Plasma-specific reader is compiled into the single
`localed-input-sources-helper` executable. No separate Plasma helper binary
or KDE development library is required; communication uses GIO/GDBus.

### Adding other desktop environments

The backend interface is intended to keep desktop-specific integration out
of the locale daemon's core. Future desktop integrations can add their
implementation behind the same interface and build-time selection. They
should reuse the single `localed-input-sources-helper` executable rather
than install a separate helper binary for each desktop.

## Project layout

```text
.
├── meson.build
├── meson_options.txt
├── src/
│   ├── main.c
│   ├── rcl-locale.c
│   ├── rcl-locale.h
│   ├── rcl-file-transaction.c
│   ├── rcl-file-transaction.h
│   ├── rcl-file-utils.h
│   ├── rcl-xkb-config.c
│   ├── rcl-xkb-config.h
│   ├── rcl-accounts-service.c
│   ├── rcl-accounts-service.h
│   └── backends/
│       ├── backend-interface.c
│       ├── backend-interface.h
│       ├── input-sources-helper.c
│       ├── gnome/
│       │   ├── gnome-backend.c
│       │   ├── gnome-backend.h
│       │   ├── gnome-input-sources-helper.c
│       │   └── gnome-input-sources-helper.h
│       └── plasma6/
│           ├── plasma6-backend.c
│           ├── plasma6-backend.h
│           ├── plasma6-input-sources-helper.c
│           └── plasma6-input-sources-helper.h
├── tests/
│   ├── test-dbus-lifecycle.c
│   ├── test-file-mode.c
│   ├── test-file-transaction.c
│   ├── test-xkb-config.c
│   ├── test-slackware-dbus-restart.sh
│   └── backends/
│       ├── gnome/
│       │   └── test-input-sources.c
│       └── plasma6/
│           └── test-input-sources.c
├── data/
│   ├── org.freedesktop.locale1.service.in
│   ├── org.freedesktop.locale1.conf
│   ├── org.freedesktop.locale1.policy
│   ├── org.freedesktop.locale1.rules.in
│   └── org.freedesktop.locale1.xml
├── man/
├── po/
└── slack/
    └── localectl
```

## D-Bus interface quick reference

Read the `Locale` property:

```sh
busctl get-property org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 Locale
```

Set the system locale (may trigger a Polkit prompt):

```sh
busctl call org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 \
       SetLocale asb 1 LANG=en_US.UTF-8 true
```

Set the X11 keyboard layout:

```sh
busctl call org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 \
       SetX11Keyboard ssssbb us pc105 "" "" false true
```

Set the console keymap and convert to X11:

```sh
busctl call org.freedesktop.locale1 \
       /org/freedesktop/locale1 org.freedesktop.locale1 \
       SetVConsoleKeyboard ssbb us "" true true
```

## Files managed

| File | Property / purpose |
|---|---|
| `/etc/profile.d/lang.sh` | `Locale` (array of `KEY=value`) |
| `/etc/profile.d/lang.csh` | C-shell companion locale settings (written alongside `lang.sh`) |
| `/etc/rc.d/rc.keymap` | `VConsoleKeymap` |
| `/etc/rc.d/rc.keymap.toggle` | `VConsoleKeymapToggle` |
| `/etc/X11/xorg.conf.d/00-keyboard.conf` | `X11Layout`, `X11Model`, `X11Variant`, `X11Options` |

The configured files are watched with inotify; `localed` re-reads them and
emits `PropertiesChanged` when their values change. The locale property is
read from Slackware's `lang.sh` script, not `/etc/locale.conf`. The console
keymap is managed through Slackware's `rc.keymap` script, not
`/etc/vconsole.conf`.

## Keyboard conversion

`SetVConsoleKeyboard` and `SetX11Keyboard` both accept a `convert`
boolean. When `true`, a built-in mapping table translates the console
keymap to an X11 layout (or vice versa). The table covers common
single-layout keymaps (`us`, `de`, `fr`, `gb`, `es`, `ru`, `se`,
etc.). No conversion is attempted when no match is found, rather than
guessing.

## AccountsService integration

When `org.freedesktop.Accounts` is available on the system bus, `localed`
subscribes to account `Language` property changes and applies them for
accounts in the configured privileged group (default: `wheel`). The
monitor is optional and reconnects when AccountsService appears or restarts.
Because D-Bus property-change signals do not identify the original caller,
this integration cannot perform caller-based Polkit authorization. Other
users should request system-wide changes through the
`org.freedesktop.locale1` D-Bus methods, which are authorized by Polkit.

## Adding a translation

1. Add the language code to `po/LINGUAS` (one code per line).
2. Generate an initial `.po` file:
   ```sh
   ninja -C build localed-pot
   msginit -l de -o po/de.po -i po/localed.pot
   ```
3. Translate the strings in `po/de.po`.
4. Rebuild; Meson compiles and installs `.mo` files automatically.
