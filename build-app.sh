#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

echo "Building SeeEverything (release)..."
swift build -c release

BIN_DIR="$(swift build -c release --show-bin-path)"
APP_DIR="$ROOT_DIR/SeeEverything.app"
CONTENTS="$APP_DIR/Contents"
MACOS="$CONTENTS/MacOS"
RESOURCES="$CONTENTS/Resources"

rm -rf "$APP_DIR"
mkdir -p "$MACOS" "$RESOURCES"

cp "$BIN_DIR/SeeEverything" "$MACOS/SeeEverything"
chmod +x "$MACOS/SeeEverything"

# Generate a clean native app icon using Swift/CoreGraphics + iconutil
ICONSET="$ROOT_DIR/.build/AppIcon.iconset"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"

swift - "$ICONSET" <<'SWIFT'
import AppKit

let outDir = CommandLine.arguments[1]
let sizes: [(Int, String)] = [
    (16, "icon_16x16.png"),
    (32, "icon_16x16@2x.png"),
    (32, "icon_32x32.png"),
    (64, "icon_32x32@2x.png"),
    (128, "icon_128x128.png"),
    (256, "icon_128x128@2x.png"),
    (256, "icon_256x256.png"),
    (512, "icon_256x256@2x.png"),
    (512, "icon_512x512.png"),
    (1024, "icon_512x512@2x.png")
]

for (px, filename) in sizes {
    let s = CGFloat(px)
    let img = NSImage(size: NSSize(width: s, height: s))
    img.lockFocus()
    let rect = NSRect(x: 0, y: 0, width: s, height: s)
    let pad = s * 0.08
    let bgRect = rect.insetBy(dx: pad, dy: pad)
    let bgPath = NSBezierPath(roundedRect: bgRect, xRadius: s * 0.22, yRadius: s * 0.22)
    let grad = NSGradient(
        starting: NSColor(calibratedRed: 0.98, green: 0.52, blue: 0.12, alpha: 1.0),
        ending: NSColor(calibratedRed: 0.88, green: 0.30, blue: 0.05, alpha: 1.0)
    )!
    grad.draw(in: bgPath, angle: -55)

    // Classic Voidtools Everything magnifying glass silhouette in crisp white
    NSColor.white.setStroke()
    let ringR = s * 0.21
    let cx = s * 0.44
    let cy = s * 0.56
    let ring = NSBezierPath(ovalIn: NSRect(x: cx - ringR, y: cy - ringR, width: ringR * 2, height: ringR * 2))
    ring.lineWidth = max(1.5, s * 0.078)
    ring.stroke()

    let handle = NSBezierPath()
    handle.move(to: NSPoint(x: cx + ringR * 0.72, y: cy - ringR * 0.72))
    handle.line(to: NSPoint(x: s * 0.76, y: s * 0.24))
    handle.lineWidth = max(2.0, s * 0.095)
    handle.lineCapStyle = .round
    handle.stroke()

    img.unlockFocus()

    if let tiff = img.tiffRepresentation,
       let rep = NSBitmapImageRep(data: tiff),
       let png = rep.representation(using: .png, properties: [:]) {
        try? png.write(to: URL(fileURLWithPath: "\(outDir)/\(filename)"))
    }
}
SWIFT

iconutil -c icns "$ICONSET" -o "$RESOURCES/AppIcon.icns" || true

cat > "$CONTENTS/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>SeeEverything</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>CFBundleIdentifier</key>
    <string>com.seeeverything.app</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>SeeEverything</string>
    <key>CFBundleDisplayName</key>
    <string>SeeEverything</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
</dict>
</plist>
PLIST

# Ad-hoc sign so TCC / Full Disk Access remembers the bundle identity
codesign --force --deep --sign - "$APP_DIR"

echo "Built $APP_DIR successfully!"
echo "CLI binary: $BIN_DIR/see-cli"
