// Draws the app icon and writes AppIcon.icns next to this file.
// Usage: swift updater/make-icon.swift
import AppKit

let here = URL(filePath: CommandLine.arguments[0]).deletingLastPathComponent()
let iconset = FileManager.default.temporaryDirectory.appending(path: "AppIcon.iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

func draw(_ size: Int) -> Data {
    let s = CGFloat(size)
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let ctx = NSGraphicsContext.current!.cgContext

    // Rounded square, inset like the system icons.
    let inset = s * 0.1
    let box = CGRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    let shape = CGPath(roundedRect: box, cornerWidth: box.width * 0.225, cornerHeight: box.width * 0.225, transform: nil)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -s * 0.012), blur: s * 0.03, color: NSColor.black.withAlphaComponent(0.35).cgColor)
    ctx.addPath(shape)
    ctx.setFillColor(NSColor.black.cgColor)
    ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(shape)
    ctx.clip()
    let colors = [NSColor(red: 0.16, green: 0.07, blue: 0.24, alpha: 1).cgColor,
                  NSColor(red: 0.55, green: 0.16, blue: 0.47, alpha: 1).cgColor,
                  NSColor(red: 0.98, green: 0.45, blue: 0.62, alpha: 1).cgColor] as CFArray
    let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colors, locations: [0, 0.6, 1])!
    ctx.drawLinearGradient(gradient, start: CGPoint(x: box.minX, y: box.minY), end: CGPoint(x: box.maxX, y: box.maxY), options: [])

    // A smooth wave across the middle, fading in and out at the ends.
    let wave = CGMutablePath()
    let left = box.minX + box.width * 0.14, right = box.maxX - box.width * 0.14
    let mid = box.midY + box.height * 0.06
    let steps = 200
    for i in 0...steps {
        let t = CGFloat(i) / CGFloat(steps)
        let x = left + (right - left) * t
        let envelope = sin(.pi * t)
        let y = mid + box.height * 0.16 * envelope * sin(t * .pi * 4)
        i == 0 ? wave.move(to: CGPoint(x: x, y: y)) : wave.addLine(to: CGPoint(x: x, y: y))
    }
    ctx.addPath(wave)
    ctx.setStrokeColor(NSColor.white.cgColor)
    ctx.setLineWidth(box.width * 0.055)
    ctx.setLineCap(.round)
    ctx.setLineJoin(.round)
    ctx.strokePath()

    // A small download arrow under the wave.
    let ax = box.midX, top = box.minY + box.height * 0.36, bottom = box.minY + box.height * 0.16
    let arrow = CGMutablePath()
    arrow.move(to: CGPoint(x: ax, y: top))
    arrow.addLine(to: CGPoint(x: ax, y: bottom))
    arrow.move(to: CGPoint(x: ax - box.width * 0.08, y: bottom + box.width * 0.08))
    arrow.addLine(to: CGPoint(x: ax, y: bottom))
    arrow.addLine(to: CGPoint(x: ax + box.width * 0.08, y: bottom + box.width * 0.08))
    ctx.addPath(arrow)
    ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.9).cgColor)
    ctx.setLineWidth(box.width * 0.045)
    ctx.strokePath()
    ctx.restoreGState()

    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for base in [16, 32, 128, 256, 512] {
    try draw(base).write(to: iconset.appending(path: "icon_\(base)x\(base).png"))
    try draw(base * 2).write(to: iconset.appending(path: "icon_\(base)x\(base)@2x.png"))
}
let task = Process()
task.executableURL = URL(filePath: "/usr/bin/iconutil")
task.arguments = ["-c", "icns", iconset.path, "-o", here.appending(path: "AppIcon.icns").path]
try task.run()
task.waitUntilExit()
print(task.terminationStatus == 0 ? "wrote AppIcon.icns" : "iconutil failed")
