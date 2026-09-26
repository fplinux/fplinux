# Installing and removing optional APK packages

Optional applications are distributed as APK files beside the FPLinux image.
Install them into an already running system; another RAM load is not required.
The application's documentation gives the exact APK filename, installed
package name, launch command and any data or shutdown requirements.

Installation affects the active system root. It lasts only for the current
session with the `default` RAM profile and persists across boots with the
`microsd-uboot` profile. Follow
[microSD system-root shutdown](MICROSD_ROOT.md#persistence-and-shutdown) before
cutting power to a persistent root.

The supported workflow installs the bundled APKs locally with `--no-network`;
no HTTPS download helper is included for APK repositories.

## Source checkout

Load the phone using the [loading guide](../guides/LOADING.md). Use the target
and profile of that running session. Find the build directory printed after
`output:` by the matching `./fplinux build` command, then upload and install the
application's APK.

This example installs `fplinux-showcase.apk` into a default
`inoi-244-modern-4g` session. Run it from the checkout root, because the
`output:` path is relative to it. Replace `<output>` with that path and
substitute the selected target, profile, output path and APK filename as one
consistent set:

```sh
./fplinux console inoi-244-modern-4g --profile default --upload \
  <output>/apks/fplinux-showcase.apk /tmp/fplinux-showcase.apk
./fplinux console inoi-244-modern-4g --profile default --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-showcase.apk'
```

Use `--profile microsd-uboot` for a session running from the microSD system
root.

## Standalone archive

Load the image using the archive's top-level `README.txt` and
[Using a standalone archive](STANDALONE.md). From the extracted archive
directory, upload and install the APK named by the application's documentation:

```sh
./runner/run.py --reconnect --upload \
  ./apks/fplinux-showcase.apk /tmp/fplinux-showcase.apk
./runner/run.py --reconnect --exec \
  'apk add --no-network --allow-untrusted --force-non-repository /tmp/fplinux-showcase.apk'
```

## Remove a package

Stop the application first. Use the installed package name from its
documentation, without the `.apk` suffix.

For the source-checkout example above:

```sh
./fplinux console inoi-244-modern-4g --profile default --exec \
  'apk del fplinux-showcase'
```

For a standalone archive:

```sh
./runner/run.py --reconnect --exec 'apk del fplinux-showcase'
```

Removing a package from the `microsd-uboot` system root is persistent. Packages
installed into the `default` RAM root disappear when that session ends even if
they are not removed explicitly.
