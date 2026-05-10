# BooConnect for Haiku

**BooConnect** is a small native Haiku GUI for OpenConnect / Cisco AnyConnect VPN connections.

It is built around a portable OpenConnect bundle and a native Haiku `BApplication` frontend. The app can prompt for password/PIN and SMS/token response codes, show the OpenConnect log, save settings locally, and launch from the Haiku Applications menu.

## Download

The ready-to-install package is included here:

```sh
dist/BooConnect-Install.zip
```

Unzip it on Haiku, then run:

```sh
BooConnect-Install/install.sh
```

The installer copies the portable package to:

```sh
/boot/home/config/non-packaged/BooConnect
```

It also creates:

```sh
/boot/home/config/non-packaged/bin/openconnect
/boot/home/config/settings/deskbar/menu/Applications/BooConnect
```

The installer does **not** include or overwrite `settings.json`, so personal VPN settings are not distributed.

## First Run

On first launch, BooConnect opens the Settings window if no configuration exists.

Typical values:

```text
Server: sslvpn.example.com
User: your.username
Interface: tun/0
User agent: AnyConnect
Script: vpnc-script
```

The default `vpnc-script` included in this repository is a generic Haiku script. It configures the tunnel interface, VPN DNS, VPN routes, and restores the local network state on disconnect.

Advanced users can use the Browse button in Settings to select a custom vpnc-script.

## Build on Haiku

```sh
make
```

This builds:

```sh
BooConnect
```

The app expects to run from:

```sh
/boot/home/config/non-packaged/BooConnect
```

because it loads `Assets/AppIcon.hvif`, `vpnc-script`, and the portable OpenConnect files relative to that directory.

## Source Layout

```text
src/main.cpp          Native Haiku GUI
Assets/AppIcon.svg    Source icon
Assets/AppIcon.hvif   Haiku vector icon
vpnc-script           Default Haiku VPN setup script
install.sh            Portable installer
dist/                 Ready installer zip
```

## Notes

- BooConnect does not run OpenConnect in background mode; it keeps the process attached so it can read prompts and write responses.
- The window close button minimizes the app. Use `File -> Quit` to disconnect and exit.
- The installer creates only the `openconnect` global symlink. The bundled `vpnc-script` is referenced from the BooConnect app directory.

## License

MIT
