#!/usr/bin/env bash
# ===========================================================================
#  BoltOS Web Engine  -  webengine/provision.sh
#
#  Run as root inside the engine's Debian VM (vm.sh setup does it). Installs
#  Firefox from Mozilla's own repository, an X server with no screen (Xvfb),
#  the Python pieces the engine uses, and the fonts Windows has, then starts
#  the engine as a service. Safe to run again.
#
#  The fonts are what makes a page look like it does on Windows:
#    - Microsoft's core web fonts, the real ones (Arial, Times New Roman,
#      Courier New, Verdana, Georgia, Trebuchet MS, Comic Sans, Impact,
#      Webdings), through Debian's ttf-mscorefonts-installer
#    - Selawik, Microsoft's open stand-in for Segoe UI, with Segoe UI's widths
#    - Cascadia Mono for Consolas, Carlito for Calibri, Caladea for Cambria
#    - Noto for emoji and for Chinese, Japanese and Korean
#  fonts.conf maps the Windows names onto them.
#
#  PulseAudio is for the sound BoltOS TV plays (Netflix and the rest): the
#  engine starts one with a sink that goes nowhere and sends what reaches it.
#  DRM video needs Widevine, which Firefox downloads by itself on x86-64
#  Linux. On arm64 (the Mac's VM) Google ships none for Firefox: those
#  services need an x86-64 machine (or VM) for the engine.
# ===========================================================================
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
SRC=/opt/boltweb

# contrib, for the core fonts' installer
if ! grep -q "contrib" /etc/apt/sources.list.d/debian.sources; then
    sed -i 's/^Components: main$/Components: main contrib/' /etc/apt/sources.list.d/debian.sources
fi
apt-get update -q
apt-get install -y -q --no-install-recommends ca-certificates curl gnupg unzip

# Firefox, the current release, from Mozilla
install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://packages.mozilla.org/apt/repo-signing-key.gpg -o /etc/apt/keyrings/packages.mozilla.org.asc
echo "deb [signed-by=/etc/apt/keyrings/packages.mozilla.org.asc] https://packages.mozilla.org/apt mozilla main" \
    > /etc/apt/sources.list.d/mozilla.list
printf 'Package: *\nPin: origin packages.mozilla.org\nPin-Priority: 1000\n' > /etc/apt/preferences.d/mozilla

echo "ttf-mscorefonts-installer msttcorefonts/accepted-mscorefonts-eula select true" | debconf-set-selections
apt-get update -q
apt-get install -y -q --no-install-recommends \
    firefox xvfb xauth x11-xserver-utils dbus-x11 libpci3 libegl1 libgl1 libgles2 \
    libavcodec61 \
    python3 python3-xlib python3-av python3-numpy python3-websockets \
    pulseaudio pulseaudio-utils \
    fontconfig ttf-mscorefonts-installer fonts-liberation fonts-crosextra-carlito \
    fonts-crosextra-caladea fonts-cascadia-code fonts-noto-color-emoji fonts-noto-cjk xcursor-themes
if [ ! -f /usr/local/share/fonts/selawik/selawk.ttf ]; then
    tmp=$(mktemp -d)
    curl -fsSL -o "$tmp/s.zip" https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip
    mkdir -p /usr/local/share/fonts/selawik
    unzip -q -o -j "$tmp/s.zip" '*.ttf' -d /usr/local/share/fonts/selawik
    rm -rf "$tmp"
fi
install -m 644 "$SRC/fonts.conf" /etc/fonts/local.conf
fc-cache -f >/dev/null

# The engine
id boltweb >/dev/null 2>&1 || useradd -m -s /bin/bash boltweb
touch /var/log/boltweb.log && chown boltweb: /var/log/boltweb.log
install -m 644 "$SRC/boltweb.service" /etc/systemd/system/boltweb.service
systemctl daemon-reload
systemctl enable boltweb
systemctl restart boltweb
firefox --version
echo "provisioned"
