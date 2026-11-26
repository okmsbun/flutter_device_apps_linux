#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
fixture="$(mktemp -d)"
trap 'rm -r -- "$fixture"' EXIT

export XDG_DATA_HOME="$fixture/data"
export XDG_CONFIG_HOME="$fixture/config"
export XDG_CACHE_HOME="$fixture/cache"
export XDG_DATA_DIRS=/usr/local/share:/usr/share
export XDG_CURRENT_DESKTOP=GNOME
export LANGUAGE=C
export LC_ALL=C.UTF-8
export DEVICE_APPS_TEST_FILE="$fixture/report.txt"

mkdir -p "$XDG_DATA_HOME"/{applications,metainfo} "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
cat > "$XDG_DATA_HOME/applications/org.example.DeviceAppsFixture.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Device Apps Linux Fixture
Comment=Fixture for Linux plugin testing
Exec=/usr/bin/true %f
Icon=$fixture/icon.png
Keywords=deviceappsfixture;
Categories=Utility;
MimeType=text/plain;
X-Flatpak=org.example.DeviceAppsFixture
EOF

cat > "$XDG_DATA_HOME/applications/org.example.HiddenFixture.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Hidden Linux Fixture
Exec=/usr/bin/true %U
Keywords=deviceappsfixture;
NoDisplay=true
Terminal=true
EOF

cat > "$XDG_DATA_HOME/applications/org.example.OnlyShowInKdeFixture.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=KDE Only Fixture
Exec=/usr/bin/true
OnlyShowIn=KDE;
EOF

cat > "$XDG_DATA_HOME/applications/org.example.NotShowInGnomeFixture.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Not In GNOME Fixture
Exec=/usr/bin/true
NotShowIn=GNOME;
EOF

cat > "$XDG_DATA_HOME/applications/org.example.TrulyHiddenFixture.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Truly Hidden Fixture
Exec=/usr/bin/true
Hidden=true
EOF

cat > "$XDG_DATA_HOME/metainfo/org.example.DeviceAppsFixture.metainfo.xml" <<'EOF'
<component type="desktop-application">
  <id>org.example.DeviceAppsFixture</id>
  <launchable type="desktop-id">org.example.DeviceAppsFixture.desktop</launchable>
  <url type="homepage">https://example.org/</url>
  <project_license>MIT</project_license>
  <developer><name>Example Team</name></developer>
  <description><p>Opens fixture text files.</p></description>
</component>
EOF

cat > "$XDG_CONFIG_HOME/mimeapps.list" <<'EOF'
[Default Applications]
text/plain=org.example.DeviceAppsFixture.desktop;
[Added Associations]
text/plain=org.example.DeviceAppsFixture.desktop;
EOF

printf '%s\n' 'int probe(void) { return 0; }' |
  clang -x c - -fPIC -shared -Wl,-soname,libSDL3.so.0 -o "$fixture/libSDL3.so.0"
mkdir -p "$fixture/ELF Fixture"
printf '%s\n' 'int probe(void);' 'int main(void) { return probe(); }' |
  clang -x c - -L"$fixture" -Wl,-rpath,'$ORIGIN/..' -l:libSDL3.so.0 -o "$fixture/ELF Fixture/elf-probe"
cat > "$XDG_DATA_HOME/applications/org.example.ELFFixture.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ELF Fixture
Exec="$fixture/ELF Fixture/elf-probe"
EOF

printf '%s\n' 'Text for a native file handler lookup.' > "$DEVICE_APPS_TEST_FILE"
printf '%s' 'iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAGUlEQVR4nGMwSFjwnxLMMGrAqAGjBgwXAwBoAi8f/niiOAAAAABJRU5ErkJggg==' |
  base64 --decode > "$fixture/icon.png"

LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a flutter test --no-pub -d linux integration_test/linux_plugin_test.dart
