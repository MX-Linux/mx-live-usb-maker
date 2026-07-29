# Code Review

**Scope:** entire codebase (`src/`, `CMakeLists.txt`, `build.sh`, `translations-desktop-file/*.sh`)
**Date:** 2026-07-27
**Verdict:** The privileged-helper allowlist model is sound, but shell-string command construction with unescaped, attacker-influenceable file paths creates real command-injection paths, and a configuration feature (custom `LUM` binary name) is silently broken by the new helper allowlist.

## Action items

1. - [x] **Shell injection via ISO path in `isantiX_mx_family`** — `src/mainwindow.cpp:898-904`
   `selected` (the ISO path) is interpolated into a single-quoted shell string (`xorriso -indev '%1' ...`) run via `Cmd::run()`/bash. This function is invoked automatically at startup whenever an ISO path is passed as `argv[1]` (see `MainWindow::MainWindow`, `mainwindow.cpp:69-74`, and `setDefaultMode`), e.g. via a file manager's "Open With" / desktop-file `%f` association — no button click needed. A filename containing a single quote (e.g. `foo'; touch pwned; echo '.iso`, which is a legal Linux filename) breaks out of the quoting and executes arbitrary commands as the invoking user simply by the user opening/selecting that file. Build the argument list and use `QProcess` with an argument vector (as `Cmd::proc`/`procAsRoot` already do elsewhere) instead of a hand-built bash string, or at minimum escape embedded `'` characters (`'\''`).

2. - [x] **Shell injection via source path in `makeUsb`/`calculateSourceSize`** — `src/mainwindow.cpp:154`, `:156`, `:157`, `:993`
   The selected ISO file / clone source directory (`source`, `sourceFilename`, `linuxfsPath` — all attacker/user-controlled paths from `QFileDialog` or the CLI arg) are interpolated into double-quoted shell strings (`du -m \"%1\"`, `df --output=source \"%1\"`, `df --output=used -B1 \"%1\"`) executed via `Cmd::getOut`. A path containing a `"` character breaks out of the quoting and injects arbitrary shell commands (e.g. selecting/opening a file named `foo".iso"; rm -rf ~; echo ".iso`). Same fix as item 1: pass arguments via `QProcess` argument lists instead of interpolating into a bash `-c` string.

3. - [x] **Configurable `LUM` binary name is broken by the privileged-helper allowlist** — `src/mainwindow.cpp:51-59` vs `src/helper.cpp:56-65`
   `mainwindow.cpp` builds `LUM` from `settings.value("LUM", "live-usb-maker")` (added specifically to let admins point at an alternate/test binary name, per the original "add .conf file option for LUM location, testing purposes" commit) and passes it to `cmd.procAsRoot(LUM, ...)` (`mainwindow.cpp:199`, `:209`). Since the helper refactor (`4e56222`), `Cmd::procAsRoot` routes through the helper's `handleExec`, which looks up the command by `QFileInfo(programPath).fileName()` in a hardcoded `allowedCommands()` map (`helper.cpp:56-65`) containing only the literal key `"live-usb-maker"`. Any admin who sets `LUM` to a different binary name in `/etc/mx-live-usb-maker/mx-live-usb-maker.conf` gets `"Command is not allowed"` (exit 127) on every privileged operation — USB creation always fails with "Error encountered in the LiveUSB creation process" instead of using the configured tool. Either derive the allowlist key from the configured `LUM` name (passed through safely) or document/remove the `LUM` override since it's now non-functional.

4. - [x] **Error dialog drops the failing path due to missing placeholder** — `src/mainwindow.cpp:732`
   `QMessageBox::critical(this, tr("Failure"), tr("Could not find linuxfs file").arg(selected));` — the translated string `"Could not find linuxfs file"` has no `%1` placeholder, so `.arg(selected)` is a no-op and the user is never shown which directory was selected. Add a placeholder, e.g. `tr("Could not find linuxfs file in %1").arg(selected)`.

5. - [ ] **Unused member variable `elevate`** — `src/mainwindow.h:80`
   `QString elevate;` is declared as a private member of `MainWindow` but never referenced anywhere in the codebase. It was presumably left over from an earlier iteration that stored the elevation tool path in the class; now elevation is handled entirely by `Cmd::elevationTool()`/`Cmd::procAsRoot()`. Remove the declaration to avoid confusion and dead-data overhead.
