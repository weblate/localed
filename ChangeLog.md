# Changelog

All notable changes to `localed` are documented in this file.

## [0.3.0] - 2026-11-20

Stable release of localed 0.3.0 for Slackware and other non-systemd systems.

### Added
- Support optional GNOME and KDE Plasma 6 input-source integration through the `desktop-backend` Meson option, with `none` as the default.
- Expand gettext/Weblate extraction to configuration errors and desktop helper diagnostics.

### Changed
- Centralize inclusion of GLib gettext macros through Meson compiler arguments.
- Preserve comma-separated X11 keyboard layouts in `localectl status` output.

### Validation
- Retain configuration rollback, XKB parsing, D-Bus lifecycle, and desktop-backend regression tests.

## [0.3.rc1] - 2026-11-15

Follow-up release candidate for localed 0.3.0.

### Fixed
- Preserve commas in D-Bus property values displayed by `localectl status`.
  The GVariant unwrapping helper no longer removes commas from values such as
  multi-layout X11 keyboard settings (for example, `us,es`).
- Remove only the trailing comma used by the GVariant tuple representation,
  without altering commas that are part of the returned property value.

## [0.3.rc] - 2026-11-09

Release candidate for localed 0.3.0.

### Added
- Add an optional KDE Plasma 6 input-source backend selected with
  `-Ddesktop-backend=plasma6`; the default remains `none`.
- Query KWin's `org.kde.KeyboardLayouts` D-Bus interface for the ordered
  layout list and active layout index, and monitor `layoutChanged` and
  `layoutListChanged` signals.
- Read per-layout variants and XKB options from the session user's
  `~/.config/kxkbrc`. Interpret KWin's `a(sss)` layout entries as
  short name, display name, and long name; do not treat display name as a
  variant.
- Compile the Plasma reader into the existing
  `localed-input-sources-helper` executable; no second helper binary or
  KDE development library is introduced.

### Tests
- Add Plasma 6 D-Bus layout tuple and helper snapshot format tests, plus
  backend start/stop lifecycle coverage. Run the Plasma-enabled Slackware
  build and test suite before considering this work complete.

## [0.3.beta]

### Fixed
- Reject configuration snapshots when the file pathname is removed or atomically
  replaced while its original inode is being read. Confirm the opened inode
  still matches the path before accepting the snapshot, and detect a file
  appearing during an initially absent-file check.
- Treat `--replace` as a startup-only instruction. After locale1 name
  acquisition, the daemon follows the normal reconnect path after a system-bus
  restart or subsequent name loss.
- Keep the sanitizer CI D-Bus integration tests isolated from host
  configuration by supplying dedicated locale, console-keymap, and Xorg
  keyboard paths.

### Tests
- Add a FIFO-based regression test that replaces a configuration pathname
  while the snapshot reader still holds the original inode open.
- Exercise `localed --replace` through an isolated system-bus restart and
  verify that locale1 ownership and the AccountsService watch are restored.
- Run the complete Meson test suite with AddressSanitizer and leak detection
  in GitHub Actions; all four test targets pass.

### Build
- Run SlackBuild tests in a separate Meson build configured with isolated
  temporary paths, then create the package from a second production build
  configured with the real Slackware paths. Test-only paths are not compiled
  into the installed daemon.
- Enable GLib deprecation diagnostics for the daemon instead of suppressing
  them at the target level.
- Run sanitizer CI on Phase 7 branch pushes and correct the stale Meson
  option documentation for the Slackware console-keymap path.

### Fixed
- Reject `TMP=/` in the SlackBuild before any cleanup operation, preventing
  package-specific staging paths from being rooted directly under the
  filesystem root.
- Handle whitespace in package paths while identifying ELF files for
  stripping and while compressing manual-page symlinks.
- Exclude local Meson build directories and Git metadata when staging
  SlackBuild sources, preventing test and debug build artifacts from being
  copied into the package build tree.
- Isolate the SlackBuild source staging directory beneath `TMP` and remove
  only that package-specific directory and its staging package. The build no
  longer deletes the entire caller-supplied `TMP` directory, which could
  otherwise remove unrelated package build trees or even all of `/tmp` when
  `TMP=/tmp`.
- Quote the SlackBuild staging, installation, documentation, and package
  paths so build paths containing whitespace are handled consistently.

### Changed
- Parse Xorg keyboard configuration through a dedicated XKB configuration
  parser. Only the `system-keyboard` InputClass contributes the XKB values
  exposed by localed; malformed owned sections are rejected while unrelated
  InputClass sections are ignored.
- Validate XKB layout, model, variant, and options values with field-specific
  rules and a shared 256-byte maximum. Use the same validation for D-Bus
  updates and values read from the Xorg configuration file.
- Match console-to-X11 keyboard conversion using both the first layout and
  its corresponding first variant, preserving positional layout/variant
  semantics instead of matching on layout alone.

### Fixed
- Reject invalid X11 keyboard values before starting PolicyKit authorization,
  avoiding unnecessary authorization requests for malformed method arguments.
- Report Xorg keyboard configuration read and parse failures in the journal
  instead of silently treating every read failure as an unconfigured file.

### Tests
- Add dedicated Xorg XKB parser tests for owned and unrelated InputClass
  sections, malformed directives, quoting, duplicate entries, CRLF input,
  and XKB field validation.
- Add D-Bus integration coverage for invalid X11 keyboard updates and
  transactional keyboard conversion, including failures at each forward
  configuration rename and verification that files and D-Bus properties
  retain their original values after rollback.

### Fixed
- Restore configuration snapshots through a sibling temporary file, apply the
  original mode before replacement, and atomically rename the restored file
  into place. Rollback no longer truncates the live destination while
  restoring its previous contents.
- Keep newly-created `/etc/profile.d/lang.sh` and
  `/etc/profile.d/lang.csh` executable with mode 0755. Add separate
  regression cases for both locale profile scripts while continuing to
  preserve the modes of existing files.

### Tests
- Add a Slackware integration harness that restarts an isolated system-bus
  instance and verifies localed remains alive, reacquires locale1, and
  independently reconnects to AccountsService.
- Add isolated PolicyKit D-Bus tests for authorization denial, interactive
  authorization flags, cancellation identifier propagation, and cancellation
  of an outstanding authorization when the daemon session shuts down.
- Replace the main daemon's shared-bus name owner with a fresh private
  system-bus connection for each locale1 ownership attempt. Register the
  locale1 object on that connection so daemon name reacquisition works after
  the system bus daemon restarts.
- Monitor the private locale1 connection's `closed` signal and route
  transport closure and name loss through one idempotent reconnect scheduler.
  Retire the old name watch and object registration before releasing the
  connection, then establish a fresh connection and register locale1 again.
  Preserve initial name-acquisition failure handling when the bus is still live.
- Document Slackware D-Bus/AccountsService restart verification procedures.


### Tests
- Add isolated GTestDBus coverage for AccountsService name appearance and
  disappearance, repeated start/stop, locale1 object registration/shutdown,
  and recovery after the system bus daemon restarts.
- Run the Meson test suite in CI with AddressSanitizer, GLib malloc
  instrumentation, and leak detection to catch memory and resource cleanup
  regressions.


### Changed
- Harden daemon startup and shutdown for repeated D-Bus registration: clear
  stale object registrations, introspection data, and authorization-session
  cancellables before reconnecting; clean up partially initialized startup
  failures.
- Make the AccountsService monitor recover from system-bus disconnections by
  creating a fresh private system-bus connection and retrying after failure.
  Stop now cancels pending retries and releases name watches, subscriptions,
  and connection callbacks.
- Send PolicyKit CheckAuthorization and CancelCheckAuthorization requests over
  localed's managed private system-bus connection instead of creating a
  separate PolkitAuthority connection. Each request uses a unique cancellation
  identifier and retains its own session cancellable and D-Bus invocation.
  Shutdown cancels pending checks, and callbacks recheck cancellation before
  applying settings to close authorization/reconnect races.


### Documentation
- Correct the README and manual pages to describe Slackware's actual locale
  and keyboard files instead of the systemd-specific `locale.conf` and
  `vconsole.conf` paths.
- Document that `localectl` uses the locale1 system D-Bus interface and
  Polkit authorization, and clarify the optional AccountsService integration
  and its authorization boundary.
- Update build and test instructions, dependencies, and the source-tree
  overview for the standalone AccountsService module and opt-in Meson tests.

### Changed
- Move AccountsService monitoring and language synchronization into a
  dedicated `RclAccountsService` GObject. The module owns its system-bus
  connection, name watch, signal subscription, reconnection, and cleanup;
  `main.c` now only starts and releases the integration.

### Fixed
- Watch the AccountsService D-Bus name and subscribe to language-property
  changes when the service appears. Unsubscribe when it disappears so the
  bridge recovers when AccountsService starts after localed or restarts.

### Security
- Clarify the AccountsService authorization boundary: property-change signals
  do not identify the original caller, so the bridge cannot perform
  caller-based Polkit authorization. Only propagate changes for accounts in
  the configured privileged group, matching the installed localed Polkit
  rule; other users must request system-wide changes through locale1.SetLocale.

### Fixed
- Treat failures reading the existing `lang.sh` as errors instead of silently
  replacing the file without preserving its existing non-locale settings. A
  missing file remains a valid first-use case.
- Make locale updates transactional across `lang.sh` and `lang.csh`. If the
  second write fails, restore the original Bourne-shell locale file.
- Make console-keymap updates transactional across `rc.keymap` and its toggle
  sidecar. If the sidecar update fails, restore the original console keymap.
- Make requested X11/console keyboard conversions transactional. A failure
  updating either side is returned to the D-Bus caller instead of being logged
  and silently reported as success.
- Refresh cached D-Bus properties after failed writes so they reflect the
  actual on-disk state, including when rollback itself cannot be completed.
- Include rollback failures in the returned error and release the associated
  error objects.

### Changed
- Preserve existing permission bits when atomically replacing `lang.sh`,
  `lang.csh`, `rc.keymap`, the keymap toggle sidecar, and the X11 keyboard
  configuration. First-time files retain their established defaults; unexpected
  destination-stat errors now fail the write instead of silently changing mode.
- Add GLib regression tests for preserving existing modes, first-create defaults,
  and failing safely when the destination cannot be inspected.
- Set the Meson project and SlackBuild default package version to `0.3.0` for
  development on the `0.3.0` branch.

## [0.3.alpha]

### Documentation
- Correct the README and manual pages to describe Slackware's actual locale
  and keyboard files instead of the systemd-specific `locale.conf` and
  `vconsole.conf` paths.
- Document that `localectl` uses the locale1 system D-Bus interface and
  Polkit authorization, and clarify the optional AccountsService integration
  and its authorization boundary.
- Update build and test instructions, dependencies, and the source-tree
  overview for the standalone AccountsService module and opt-in Meson tests.

### Changed
- Move AccountsService monitoring and language synchronization into a
  dedicated `RclAccountsService` GObject. The module owns its system-bus
  connection, name watch, signal subscription, reconnection, and cleanup;
  `main.c` now only starts and releases the integration.

### Fixed
- Watch the AccountsService D-Bus name and subscribe to language-property
  changes when the service appears. Unsubscribe when it disappears so the
  bridge recovers when AccountsService starts after localed or restarts.

### Security
- Clarify the AccountsService authorization boundary: property-change signals
  do not identify the original caller, so the bridge cannot perform
  caller-based Polkit authorization. Only propagate changes for accounts in
  the configured privileged group, matching the installed localed Polkit
  rule; other users must request system-wide changes through locale1.SetLocale.

### Fixed
- Treat failures reading the existing `lang.sh` as errors instead of silently
  replacing the file without preserving its existing non-locale settings. A
  missing file remains a valid first-use case.
- Make locale updates transactional across `lang.sh` and `lang.csh`. If the
  second write fails, restore the original Bourne-shell locale file.
- Make console-keymap updates transactional across `rc.keymap` and its toggle
  sidecar. If the sidecar update fails, restore the original console keymap.
- Make requested X11/console keyboard conversions transactional. A failure
  updating either side is returned to the D-Bus caller instead of being logged
  and silently reported as success.
- Refresh cached D-Bus properties after failed writes so they reflect the
  actual on-disk state, including when rollback itself cannot be completed.
- Include rollback failures in the returned error and release the associated
  error objects.

### Changed
- Preserve existing permission bits when atomically replacing `lang.sh`,
  `lang.csh`, `rc.keymap`, the keymap toggle sidecar, and the X11 keyboard
  configuration. First-time files retain their established defaults; unexpected
  destination-stat errors now fail the write instead of silently changing mode.
- Add GLib regression tests for preserving existing modes, first-create defaults,
  and failing safely when the destination cannot be inspected.
- Set the Meson project and SlackBuild default package version to `0.3.0` for
  development on the `0.3.0` branch.

## [0.2.1] - 2026-10-21

### Changed
- Write locale settings to both `/etc/profile.d/lang.sh` and
  `/etc/profile.d/lang.csh` so Bourne-compatible and C-shell logins use the
  configured locale.

### Maintenance
- Remove the obsolete `BUGS-NEWS` file; its contents predate the 0.2.0 branch.

## [0.2.0] - Unreleased

### Fixed
- Exit when the daemon loses ownership of the D-Bus name. D-Bus activation can
  start a fresh instance; the running object is no longer restarted in place.
- Reject locale and keyboard values longer than 256 bytes, matching the
  readers' limits so accepted values can be read back unchanged.
- Parse X11 keyboard options only from actual `Option "Name" "value"` lines;
  comments and unrelated text mentioning an option are ignored.
- Make the logging macro safe in conditional contexts and make debug filtering
  explicit.
- Watch `/etc/rc.d/rc.keymap.toggle` as well as the locale, console-keymap,
  and X11 keyboard files.
- Debounce bursts of file-change events before synchronizing D-Bus properties.
- Clarify that `SetLocale` replaces the complete locale set.

### Changed
- Restrict the default D-Bus policy to Introspectable, Properties, and the
  `org.freedesktop.locale1` interface; remove the blanket destination allow
  and unused ObjectManager/receive-sender rules.
- Remove the obsolete direct-file read/write implementation from
  `slack/localectl`. The client uses the D-Bus service for status and changes.
- Stop installing `localectl` a second time in the SlackBuild; Meson owns
  its installation.
- Keep the release notes focused on current 0.2.0 behavior; older fixed-bug
  inventories are maintained in version control rather than repeated here.

## [0.1.12] - 2026-07-11

### Security

- **`rcl_read_rc_keymap_toggle()` returned sidecar content without validation**
  (`src/rcl-locale.c`): the toggle sidecar file was read, whitespace-stripped,
  and returned verbatim with no allowlist check, unlike every other keyboard
  string field.  A local user who could write
  `/etc/rc.d/rc.keymap.toggle` was able to inject arbitrary content into the
  `VConsoleKeymapToggle` D-Bus property.  Fixed by applying
  `rcl_keyboard_value_is_safe()` after stripping; values outside
  `[A-Za-z0-9\-_.,+:]` are reset to `""` with a `g_warning`, matching the
  `SANITISE_XKB` pattern used for X11 fields and the safe-charset check in
  `rcl_read_rc_keymap()`.

- **`rcl_write_lang_sh()` and `rcl_write_rc_keymap_toggle()` created files
  mode 0600** (`src/rcl-locale.c`): `g_file_set_contents()` creates its temp
  file with mode 0600.  When localed wrote `lang.sh` or the toggle sidecar
  from scratch (first boot or file deleted), the resulting file was readable
  only by root, silently breaking locale for all login shells.  Fixed by
  calling `g_chmod(tmp_path, 0644)` between `g_file_set_contents()` and
  `g_rename()` in both writers, mirroring the existing `g_chmod(tmp_path, 0755)`
  call in `rcl_write_rc_keymap()`.

- **Dead `kv_read()` / `kv_set()` functions removed** (`slack/localectl`):
  both functions were defined but never called.  `kv_set()` used
  `grep -v "^${key}="` with an unsanitised `$key`, making it vulnerable to
  regex injection if it were ever invoked with caller-controlled input.
  Removed; `rcl_lang_sh_get()` and `rcl_write_lang_sh()` cover the same
  roles with proper validation.

## [0.1.11] - 2026-06-28

### Fixed

- **`keyboard_value_is_safe()` in `localectl` out of sync with C daemon**:
  the bash validator still used the old blocklist (reject `"`, `\`, and
  control chars) while `rcl_keyboard_value_is_safe()` in C was tightened to
  a strict allowlist in 0.1.9. Shell metacharacters (`$`, `` ` ``, `!`,
  `(`, `)`, `;`, `|`, `&`, `>`, `<`, `*`, `?`, space, etc.) were still
  accepted by the bash script. Replaced with an allowlist regex
  `^[A-Za-z0-9._,:+:-]+$` matching the C implementation exactly.

- **`do_set_locale` locale array had no upper-bound check**: the validation
  loop iterated the incoming `locale` GStrv with no limit. An authorised
  client could send an array with thousands of entries, forcing O(N×14)
  `strncmp` calls before PolKit even influenced the response. Added a
  pre-validation entry count check; arrays exceeding the number of known
  locale variables (14) are rejected immediately with
  `G_DBUS_ERROR_INVALID_ARGS`.

- **Locale values read from `lang.sh` not validated against safe charset**:
  `rcl_read_lang_sh()` stored values from disk directly into the `Locale`
  D-Bus property without checking them against `rcl_locale_value_is_safe()`.
  A manually-edited `lang.sh` with characters outside `[A-Za-z0-9_.@:,-]`
  could expose unsafe strings as D-Bus property values. Values that fail
  validation are now silently skipped with a `g_warning`.

- **`rcl_locale_daemon_finalize` did not clear `connection` or
  `registration_id`**: if the GObject were finalized without an explicit
  `rcl_daemon_shutdown()` call (e.g. on a startup error path), the
  `GDBusConnection` reference would leak. Added the same unregister-and-clear
  block to `finalize` that `rcl_daemon_shutdown` already performed, guarded
  by `if (connection && registration_id > 0)`.

- **`do_set_locale` key allocation fragile under refactor**: manual
  `g_free(key)` calls on two early-return branches were replaced with
  `g_autofree gchar *key`, making the allocation leak-proof on all current
  and future return paths without any explicit `g_free`.

- **`rcl_write_lang_sh` hash table ownership contract undocumented**:
  switched from `g_hash_table_new()` to `g_hash_table_new_full(..., NULL, NULL)`
  and added a comment explicitly documenting that keys and values are
  borrowed pointers and must not be freed by the destructor. Prevents silent
  double-free if a future caller passes heap-allocated strings.

- **`list-keymaps` and `list-locales` grep filter not fixed-string**:
  both commands piped user-supplied filter arguments to `grep -i`, allowing
  POSIX ERE metacharacters and `--option`-style injection. Changed to
  `grep -iF` (fixed-string) in both commands.

- **Preserved `cur_val` in `localectl rcl_write_lang_sh` not validated**:
  when `set-locale` updated a subset of locale variables, values preserved
  from the existing `lang.sh` for untouched variables were re-emitted
  without validation. A manually-edited `lang.sh` entry with unsafe
  characters would be silently propagated on the next write. Preserved
  values are now checked with `locale_value_is_safe`; unsafe values are
  dropped rather than re-emitted.

- **XKB values read from `00-keyboard.conf` not charset-validated**:
  `read_x11_keyboard_conf()` passed all four XKB field values through
  `extract_xkb_option()` (which caps length) but did not validate the
  character set. A manually-edited config with characters outside
  `[A-Za-z0-9\-_.,+:]` would be served as D-Bus property values.
  Added a `SANITISE_XKB` macro that calls `rcl_keyboard_value_is_safe()`
  on each field after extraction, resetting unsafe values to empty string
  with a `g_warning`.


## [0.1.10] - 2026-06-28

### Fixed

- **Keymap read-back broken after quoting fix** (`rcl_read_rc_keymap` in C
  and `localectl`): fix 8 (0.1.9) quoted the keymap in the generated
  `rc.keymap` script (`/usr/bin/loadkeys "us"`), but the reader extracted
  the last whitespace-delimited token verbatim, returning `"us"` with quotes
  instead of `us`. `VConsoleKeymap` D-Bus property values were therefore
  wrong after any write, breaking clients such as GNOME Control Center that
  compare the property against known keymap names. Fixed in `rcl_read_rc_keymap()`
  (C) by stripping surrounding double-quotes after extraction; fixed in
  `localectl` by running `gsub(/^"|"$/, "", tok)` in the awk script.

- **Fragile `locale[0]` alias in `do_set_locale`**: the bare-locale shorthand
  path (`SetLocale(["en_US.UTF-8"])`) stored a `bare` alias to `locale[0]`,
  then replaced `locale[0]` with the expanded form and freed `bare`. Correct
  today but would produce a dangling pointer if an early-return path were
  added after the `g_free`. Replaced with an explicit `expanded` variable;
  `locale[0]` is now freed directly without an intermediate alias.

- **`localtime()` not thread-safe in log handler** (`main.c`): the
  colourised terminal log handler called `localtime(&t)` which returns a
  pointer to a static buffer — not safe if GLib ever invokes the handler
  from a thread-pool callback. Replaced with `localtime_r(&t, &tm_buf)`
  using a stack-allocated `struct tm`.

- **Redundant `local` declarations inside `while` loop** (`localectl`
  `rcl_write_lang_sh`): `local stripped`, `local rest`, and `local lkey`
  were declared inside the `while IFS= read -r line` loop body, shadowing
  same-named variables in the outer function scope. Removed the `local`
  qualifiers; the variables are now plain assignments scoped to the
  enclosing function as intended.


## [0.1.9] - 2026-06-27

### Security

- **`localectl set-locale` wrote to wrong file**: `cmd_set_locale` called
  `kv_set` targeting `/etc/locale.conf`, which the daemon never reads.
  The daemon reads and writes `/etc/profile.d/lang.sh` exclusively. Fixed
  by replacing `kv_set` with a new `rcl_write_lang_sh()` function that
  writes `export KEY="value"` format to the correct path.

- **`LANG_SH_FILE` undefined in `localectl`**: `cmd_status` referenced
  `$LANG_SH_FILE` before it was assigned, causing the locale section of
  `localectl status` to always display `n/a`. Fixed by defining
  `LANG_SH_FILE="/etc/profile.d/lang.sh"` at the top of the script.

- **`cmd_set_x11_keymap` missing from `localectl`**: the dispatch table
  referenced `cmd_set_x11_keymap` but the function was never defined,
  causing `localectl set-x11-keymap` to always fail with
  `command not found`. Function implemented with full argument handling,
  read-current-values semantics for omitted fields, validation, and
  `--no-convert` support.

- **XKB option value length not bounded**: `extract_xkb_option()` had no
  length cap on values parsed from the X11 keyboard config file. Added
  `#define MAX_XKB_VALUE_LEN 256`; values exceeding this are silently
  ignored and fall back to empty string, with a `g_warning` logged.

- **Temp file leaked on rename failure**: all four atomic write functions
  (`write_x11_keyboard_conf`, `rcl_write_rc_keymap`,
  `rcl_write_rc_keymap_toggle`, `rcl_write_lang_sh`) failed to unlink the
  temp file when `g_rename()` returned an error, leaving stale `.tmp` files
  on disk. Fixed by adding `g_unlink(tmp_path)` in each rename-failure
  branch.

- **Shell injection in `rcl_write_rc_keymap` (C daemon)**: keymap values
  written into `/etc/rc.d/rc.keymap` were interpolated unquoted into the
  generated shell script. A value such as `us $(reboot)` would pass the
  previous validator and execute `reboot` as root when `rc.keymap` is
  sourced at boot. Values are now written as `/usr/bin/loadkeys "<keymap>"`.

- **Shell injection in `localectl rcl_write_x11_keyboard`**: the X11
  keyboard config was written via a double-quoted heredoc, allowing
  `$(...)`-style expansion of layout, model, variant, and options values.
  Replaced with a single-quoted heredoc (`<< 'HEREDOC'`) for the static
  skeleton and `printf '%s'` for each variable field. Combined with the
  tightened validator below, injection is blocked at two independent layers.

- **Overly permissive keyboard value validator**: `rcl_keyboard_value_is_safe()`
  previously denied only `"`, `\`, and control characters, leaving shell
  metacharacters (`$`, `` ` ``, `!`, `(`, `)`, `;`, `|`, `&`, `>`, `<`,
  `*`, `?`, space, etc.) as accepted input that could reach `rc.keymap` or
  `xorg.conf.d`. The validator now uses a strict allowlist:
  `[A-Za-z0-9\-_.,+:]` only — sufficient for all real keymap and XKB
  identifiers. Applied in both the C daemon and `localectl`.

- **Undefined behaviour in `rcl_write_rc_keymap_toggle`**: the function
  had no `return` statement on the write path, making the caller's use of
  the `gboolean` result undefined behaviour under C99/C11. Fixed by adding
  `return ok` and the matching `g_free` calls before it.

### Changed

- Documented the trust assumption in `rcl_write_lang_sh`: non-locale lines
  in `lang.sh` are preserved verbatim on the basis that `lang.sh` is a
  root-owned system file. Any caller that sources user-supplied content into
  `lang.sh` before invoking this function is responsible for sanitising it.


## [0.1.8] - 2026-06-22

### Changed (`localectl status`)
- Output now uses right-aligned 16-character label fields matching systemd
  localectl exactly:
  ```
     System Locale: LANG=en_US.UTF-8
                    LC_TIME=en_GB.UTF-8
         VC Keymap: us
        X11 Layout: cz,us
         X11 Model: pc104
       X11 Variant: ,dvorak
       X11 Options: grp:win_space_toggle
  ```
- Multiple locale variables are shown with the first on the label line and
  continuations indented to the value column (18 spaces).
- X11 Model, Variant, and Options are always shown when a layout is set,
  even when empty (shown as `-`), matching upstream behaviour.
- X11 value reading replaced with portable awk (`rcl_x11_read_opt`) instead
  of `grep -oP` lookbehind, which failed on non-PCRE grep builds.
- Locale reading replaced with `rcl_lang_sh_get` (awk-based) for the same
  portability reason.

### Added (`localectl`)
- `--no-pager` global flag: suppresses piping list command output through
  `$PAGER` (default: `less -FRX`). Output is also never paged when stdout
  is not a terminal. Applied to all six `list-*` commands.


## [0.1.7] - 2026-06-22

### Changed (`localectl`)
- `set-x11-keymap LAYOUT [MODEL [VARIANT [OPTIONS]]]` now reads the current
  values from the X11 keyboard config file and uses them as defaults for any
  argument not supplied on the command line.  This allows updating a single
  field without repeating all the others:
    localectl set-x11-keymap cz,us pc104   # keeps existing variant and options
  Passing an explicit empty string `""` still clears that field:
    localectl set-x11-keymap cz,us "" "" grp:alt_shift_toggle
  The distinction is made via `${N-default}` (unset) vs `${N:-default}`
  (unset or empty), so the behaviour is unambiguous.
- `set-x11-keymap` and `set-keymap` now perform bidirectional conversion by
  default (matching systemd localectl behaviour): `set-x11-keymap` also
  updates `/etc/rc.d/rc.keymap` and `set-keymap` also updates the X11
  keyboard config, using the same 26-entry conversion table as the daemon.
- X11 keyboard config parser correctly handles tab-indented `Option` lines
  and multi-layout values such as `"cz,us"` and `",dvorak"`.

### Added (`localectl`)
- `--no-convert` flag for `set-keymap` and `set-x11-keymap`: suppresses the
  automatic cross-conversion, writing only the target file.
- `rcl_write_x11_keyboard` internal helper extracted so both `set-x11-keymap`
  and the convert path in `set-keymap` share one write implementation.
- Bash conversion table (`_vc_to_x11` / `_x11_to_vc`) mirroring the C
  daemon's `keymap_map[]` table exactly, including first-layout extraction
  from comma-separated layout strings like `"cz,us"`.


## [0.1.6] - 2026-06-22

### Added (`localectl`)
- `list-x11-keymap-models [FILTER]` — lists X11 keyboard models from
  `/usr/share/X11/xkb/rules/base.lst` (`! model` section).
- `list-x11-keymap-layouts [FILTER]` — lists X11 keyboard layouts
  (`! layout` section).
- `list-x11-keymap-variants [LAYOUT] [FILTER]` — lists X11 keyboard
  variants (`! variant` section).  When `LAYOUT` is given, restricts output
  to variants whose description begins with `LAYOUT:`, matching upstream
  systemd `localectl` behaviour.
- `list-x11-keymap-options [FILTER]` — lists X11 keyboard options
  (`! option` section), e.g. compose key assignments and Ctrl placement.

All four commands parse `base.lst` with portable POSIX awk (no gawk
dependency), optionally filter results case-insensitively, and require no
root. They match the full command set of systemd 257's `localectl`.


## [0.1.5] - 2026-06-22

### Added (`localectl`)
- `list-keymaps [FILTER]` subcommand: scans `/usr/share/kbd/keymaps/`
  recursively for `*.map.gz` / `*.map` files, strips path and extension to
  produce bare keymap names, sorts and deduplicates, then optionally pipes
  through `grep -i FILTER`.  Does not require root.
  Examples: `localectl list-keymaps`, `localectl list-keymaps de`,
  `localectl list-keymaps dvorak`.


## [0.1.4] - 2026-06-22

### Changed
- **Slackware locale support**: replaced `/etc/locale.conf` (freedesktop
  standard not used by Slackware) with `/etc/profile.d/lang.sh` as the
  backing file for the `Locale` D-Bus property.
  - `rcl_read_lang_sh()` parses `export KEY="value"` lines, strips the
    `export` prefix and quotes, ignores non-locale exports, and returns only
    known locale variables.
  - `rcl_write_lang_sh()` performs a read-modify-write: preserves comments
    and non-locale exports verbatim, then rewrites locale variable lines at
    the end in canonical variable order (`LANG`, `LANGUAGE`, `LC_*`).
    Writes are atomic (temp file + rename). The directory is created if
    absent.
  - The meson option `locale_conf` has been renamed to `lang_sh`
    (default: `/etc/profile.d/lang.sh`).
  - The now-unused `read_key_value_pairs()` and `write_key_value_file()`
    internal helpers have been removed; dead code reduction.

### Added (`localectl`)
- `list-locales [FILTER]` subcommand: runs `locale -a` (always available
  on Slackware via glibc) and optionally pipes through `grep -i FILTER`
  so users can narrow the output. Does not require root.
  Examples: `localectl list-locales`, `localectl list-locales en_US`,
  `localectl list-locales UTF-8`.


## [0.1.3] - 2026-06-22

### Changed
- **Slackware keyboard support**: replaced `/etc/vconsole.conf` (a systemd /
  freedesktop convention not used by Slackware) with `/etc/rc.d/rc.keymap`
  as the backing file for the `VConsoleKeymap` D-Bus property.
  - `localed` now parses the `loadkeys` argument from `rc.keymap` on startup
    and via inotify whenever the file changes.
  - `SetVConsoleKeyboard` regenerates `rc.keymap` atomically (temp + rename)
    and marks it executable (0755) so Slackware's init picks it up.
  - `VConsoleKeymapToggle` is persisted in `/etc/rc.d/rc.keymap.toggle`
    (a small sidecar file with the bare keymap name) since Slackware has no
    native toggle-keymap concept. The file is created on set and removed on
    clear.
  - The meson option `vconsole_conf` has been renamed to `rc_keymap`
    (default: `/etc/rc.d/rc.keymap`). The toggle sidecar path is derived
    automatically as `<rc_keymap>.toggle`.
  - `localectl set-keymap` now calls `rcl_write_rc_keymap()` /
    `rcl_write_rc_keymap_toggle()` instead of writing a KEY=value file.
  - `localectl status` now reads the keymap via `rcl_read_rc_keymap()` /
    `rcl_read_rc_keymap_toggle()`.


## [0.1.2] - 2026-06-22

### Fixed
- `SetLocale` now auto-derives `LANGUAGE` from `LANG` when `LANGUAGE` is not
  provided in the call, matching upstream systemd-localed behaviour (gap 1 vs
  systemd 257). The derivation uses the same 13-entry language-fallback table
  as upstream (verbatim copy of systemd's `src/locale/language-fallback-map`),
  embedded directly rather than read from a runtime file.
  Example: `SetLocale(["LANG=en_AU"], true)` now also writes
  `LANGUAGE=en_AU:en_GB` to `/etc/locale.conf`.
  The match is exact (bare locale code without encoding suffix), identical to
  upstream — `LANG=en_AU.UTF-8` does NOT trigger the fallback, which is
  intentional and matches the upstream table's key format.


## [0.1.1] - 2026-06-21

### Fixed
- `SetLocale` now accepts a single bare locale string with no `KEY=` prefix
  and treats it as `LANG=<value>`, matching upstream systemd-localed behaviour
  (gap 2 vs systemd 257). Example: `SetLocale(["en_US.UTF-8"], true)` now
  correctly sets `LANG=en_US.UTF-8` instead of returning an error.


## [0.1.0] - 2026-06-21

### Added
- Initial project scaffolding: `main.c`, `rcl-locale.c`/`.h`,
  `meson.build`, `meson_options.txt`, `README.md`
- D-Bus interface description (`org.freedesktop.locale1.xml`),
  service activation file (`.service.in`), polkit policy (`.policy`),
  and polkit rules template (`.rules.in`)
- SlackBuild packaging script
- `localectl` companion shell script (writes files directly; localed
  picks up changes via inotify)
- Full `org.freedesktop.locale1` D-Bus interface:
  - Properties: `Locale` (as), `VConsoleKeymap`, `VConsoleKeymapToggle`,
    `X11Layout`, `X11Model`, `X11Variant`, `X11Options`
  - Methods: `SetLocale`, `SetVConsoleKeyboard`, `SetX11Keyboard`
- inotify watches on `/etc/locale.conf`, `/etc/vconsole.conf`, and
  `/etc/X11/xorg.conf.d/00-keyboard.conf`; `PropertiesChanged` emitted
  on any file change
- Asynchronous PolKit authorisation (agent dialog never stalls the main loop)
- Built-in console-keymap ↔ X11 layout conversion table used when
  `convert=true` is passed to `SetVConsoleKeyboard` or `SetX11Keyboard`
- Input validation for locale values and keyboard strings
- Atomic file writes (temp file + rename) for all three config files
- Reconnect/name-lost D-Bus ownership pattern (survives dbus-daemon
  restarts without the service going permanently dark)
- Colourised interactive logging and syslog(3) daemon-context logging
- Man pages: `localed(8)` and `localectl(1)`
- GPLv2 license file
