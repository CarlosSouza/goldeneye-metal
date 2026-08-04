// Generates the iPad app icon: a dark slate square with a golden
// barrel-inspired ring set and a fine crosshair. Abstract on purpose - no
// trademarked art. Usage: swift generate-icon.swift <out-dir>
// Writes AppIcon60x60@2x.png (120), AppIcon76x76@2x.png (152), AppIcon1024.png.

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

func drawIcon(size: Int) -> CGImage? {
    let s = CGFloat(size)
    guard let ctx = CGContext(data: nil, width: size, height: size,
                              bitsPerComponent: 8, bytesPerRow: 0,
                              space: CGColorSpace(name: CGColorSpace.sRGB)!,
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    else { return nil }

    // Background: near-black slate with a subtle radial lift at the center.
    ctx.setFillColor(CGColor(red: 0.055, green: 0.06, blue: 0.075, alpha: 1))
    ctx.fill(CGRect(x: 0, y: 0, width: s, height: s))
    let gradient = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
                              colors: [CGColor(red: 0.13, green: 0.13, blue: 0.15, alpha: 1),
                                       CGColor(red: 0.055, green: 0.06, blue: 0.075, alpha: 0)] as CFArray,
                              locations: [0, 1])!
    ctx.drawRadialGradient(gradient,
                           startCenter: CGPoint(x: s * 0.5, y: s * 0.5), startRadius: 0,
                           endCenter: CGPoint(x: s * 0.5, y: s * 0.5), endRadius: s * 0.62,
                           options: [])

    let center = CGPoint(x: s * 0.5, y: s * 0.5)
    let gold = CGColor(red: 0.83, green: 0.68, blue: 0.28, alpha: 1)
    let goldDim = CGColor(red: 0.83, green: 0.68, blue: 0.28, alpha: 0.45)

    // Barrel-inspired ring set: one heavy ring plus two thin companions.
    func ring(_ radius: CGFloat, _ width: CGFloat, _ color: CGColor) {
        ctx.setStrokeColor(color)
        ctx.setLineWidth(width)
        ctx.strokeEllipse(in: CGRect(x: center.x - radius, y: center.y - radius,
                                     width: radius * 2, height: radius * 2))
    }
    ring(s * 0.335, s * 0.055, gold)
    ring(s * 0.245, s * 0.014, goldDim)
    ring(s * 0.425, s * 0.014, goldDim)

    // Fine crosshair through the whole tile, gapped inside the heavy ring.
    ctx.setStrokeColor(goldDim)
    ctx.setLineWidth(s * 0.012)
    let gap = s * 0.30
    for (from, to) in [(CGPoint(x: center.x, y: s * 0.06), CGPoint(x: center.x, y: center.y - gap)),
                       (CGPoint(x: center.x, y: center.y + gap), CGPoint(x: center.x, y: s * 0.94)),
                       (CGPoint(x: s * 0.06, y: center.y), CGPoint(x: center.x - gap, y: center.y)),
                       (CGPoint(x: center.x + gap, y: center.y), CGPoint(x: s * 0.94, y: center.y))] {
        ctx.move(to: from)
        ctx.addLine(to: to)
        ctx.strokePath()
    }

    // Center dot.
    ctx.setFillColor(gold)
    let dot = s * 0.035
    ctx.fillEllipse(in: CGRect(x: center.x - dot, y: center.y - dot, width: dot * 2, height: dot * 2))

    return ctx.makeImage()
}

func write(_ image: CGImage, to url: URL) {
    guard let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString,
                                                     1, nil) else { fatalError("dest \(url)") }
    CGImageDestinationAddImage(dest, image, nil)
    guard CGImageDestinationFinalize(dest) else { fatalError("write \(url)") }
}

let outDir = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ".")
for (name, size) in [("AppIcon60x60@2x.png", 120), ("AppIcon76x76@2x.png", 152),
                     ("AppIcon1024.png", 1024)] {
    guard let image = drawIcon(size: size) else { fatalError("draw \(size)") }
    write(image, to: outDir.appendingPathComponent(name))
    print("wrote \(name)")
}
