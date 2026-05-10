#!/bin/sh
set -e

SOURCE_DIR=$(cd "$(dirname "$0")" && pwd)
TARGET_DIR=/boot/home/config/non-packaged/BooConnect
BIN_DIR=/boot/home/config/non-packaged/bin
APPS_DIR=/boot/home/config/non-packaged/apps
DESKBAR_APPS_DIR=/boot/home/config/settings/deskbar/menu/Applications
APP_NAME=BooConnect
APP_SIG=application/x-vnd.BooConnect

mkdir -p "$TARGET_DIR" "$BIN_DIR" "$APPS_DIR" "$DESKBAR_APPS_DIR"

for item in "$SOURCE_DIR"/* "$SOURCE_DIR"/.[!.]* "$SOURCE_DIR"/..?*; do
	[ -e "$item" ] || continue
	name=$(basename "$item")
	[ "$name" = "." ] && continue
	[ "$name" = ".." ] && continue
	if [ "$SOURCE_DIR" = "$TARGET_DIR" ]; then
		break
	fi
	if [ "$name" = "settings.json" ] && [ -f "$TARGET_DIR/settings.json" ]; then
		continue
	fi
	rm -rf "$TARGET_DIR/$name"
	cp -R "$item" "$TARGET_DIR/"
done

chmod +x "$TARGET_DIR/sbin/openconnect"
chmod +x "$TARGET_DIR/vpnc-script"
chmod +x "$TARGET_DIR/$APP_NAME"

ln -sf "$TARGET_DIR/sbin/openconnect" "$BIN_DIR/openconnect"
rm -f "$BIN_DIR/vpnc-script-haiku"
ln -sf "$TARGET_DIR/$APP_NAME" "$APPS_DIR/$APP_NAME"
ln -sf "$APPS_DIR/$APP_NAME" "$DESKBAR_APPS_DIR/$APP_NAME"
rm -f "$APPS_DIR/BooConnectGui" "$DESKBAR_APPS_DIR/BooConnectGui"

if [ -f "$TARGET_DIR/Assets/AppIcon.hvif" ]; then
	mimeset -f "$TARGET_DIR/$APP_NAME" >/dev/null 2>&1 || true
	addattr -t mime BEOS:APP_SIG "$APP_SIG" "$TARGET_DIR/$APP_NAME" >/dev/null 2>&1 || true
	addattr -f "$TARGET_DIR/Assets/AppIcon.hvif" -c VICN BEOS:ICON "$TARGET_DIR/$APP_NAME" >/dev/null 2>&1 || true
fi

ifconfig tun/0 up >/dev/null 2>&1 || true
open "$TARGET_DIR" >/dev/null 2>&1 || true

case "$SOURCE_DIR" in
	"$TARGET_DIR"|/|/boot|/boot/home|/boot/home/Desktop|/boot/home/Downloads|/boot/home/config|/boot/home/config/non-packaged)
		;;
	*)
		if [ -f "$SOURCE_DIR/install.sh" ] && [ -f "$SOURCE_DIR/$APP_NAME" ]; then
			cd /boot/home
			rm -rf "$SOURCE_DIR"
		fi
		;;
esac

echo "BooConnect installed at $TARGET_DIR"
echo "openconnect -> $TARGET_DIR/sbin/openconnect"
echo "Applications -> $DESKBAR_APPS_DIR/$APP_NAME"
