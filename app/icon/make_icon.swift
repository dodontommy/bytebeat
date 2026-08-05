/* make_icon.swift -- renders the MORGUE app icon set in the design language
 * of MORGUE_UI_SPEC.md: matte black plate, hairline frame, registration
 * crosses, stencil M (IBM Plex Sans Condensed Bold) in ink, one blood rule,
 * evidence serial. Run:
 *   swift app/icon/make_icon.swift <fontsDir> <outIconsetDir>
 * then: iconutil -c icns <outIconsetDir> -o MORGUE.icns
 */
import AppKit
import CoreText

let args = CommandLine.arguments
guard args.count == 3 else {
    fputs("usage: make_icon.swift <fontsDir> <outIconsetDir>\n", stderr)
    exit(2)
}
let fontsDir = args[1]
let outDir = args[2]

func rgb(_ hex: UInt32) -> CGColor {
    CGColor(red: CGFloat((hex >> 16) & 0xff) / 255.0,
            green: CGFloat((hex >> 8) & 0xff) / 255.0,
            blue: CGFloat(hex & 0xff) / 255.0, alpha: 1)
}
let GROUND = rgb(0x0a0a0a), PANEL = rgb(0x0c0b0a), EDGE = rgb(0x3a3833)
let INK = rgb(0xded9ce), INK_FAINT = rgb(0x55524b), GHOST = rgb(0x3a3833)
let BLOOD = rgb(0x8b1e14), BLOOD_HOT = rgb(0xc2301f)

func loadFont(_ file: String, _ size: CGFloat) -> CTFont {
    let url = URL(fileURLWithPath: fontsDir + "/" + file) as CFURL
    guard let descs = CTFontManagerCreateFontDescriptorsFromURL(url) as? [CTFontDescriptor],
          let d = descs.first else {
        fputs("cannot load font \(file)\n", stderr); exit(1)
    }
    return CTFontCreateWithFontDescriptor(d, size, nil)
}

/* draw one attributed line centred at (cx, cy) using glyph path bounds */
func drawCentred(_ ctx: CGContext, _ text: String, _ font: CTFont,
                 _ color: CGColor, _ tracking: CGFloat, cx: CGFloat, cy: CGFloat) {
    let attrs: [NSAttributedString.Key: Any] = [
        .font: font, .foregroundColor: color, .kern: tracking,
    ]
    let line = CTLineCreateWithAttributedString(
        NSAttributedString(string: text, attributes: attrs))
    let b = CTLineGetBoundsWithOptions(line, .useGlyphPathBounds)
    ctx.textPosition = CGPoint(x: cx - b.midX, y: cy - b.midY)
    CTLineDraw(line, ctx)
}

func drawIcon(_ n: Int) -> CGImage {
    let s = CGFloat(n) / 1024.0
    let cs = CGColorSpaceCreateDeviceRGB()
    let ctx = CGContext(data: nil, width: n, height: n, bitsPerComponent: 8,
                        bytesPerRow: 0, space: cs,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.setAllowsAntialiasing(true)

    /* macOS icon-grid plate: 824x824 rounded square centred on the 1024 canvas.
     * The corner radius is the ONE rounding in the whole product -- the OS
     * icon shape, not ours. Everything inside is square. */
    let plate = CGRect(x: 100 * s, y: 100 * s, width: 824 * s, height: 824 * s)
    let shape = CGPath(roundedRect: plate, cornerWidth: 185 * s,
                       cornerHeight: 185 * s, transform: nil)
    ctx.addPath(shape)
    ctx.clip()
    ctx.setFillColor(GROUND)
    ctx.fill(plate)

    /* raised-plate edge: keep >= 1 device px at every size */
    let hair = max(6 * s, 1)
    ctx.setStrokeColor(EDGE)
    ctx.setLineWidth(hair)
    ctx.addPath(CGPath(roundedRect: plate.insetBy(dx: hair / 2, dy: hair / 2),
                       cornerWidth: 182 * s, cornerHeight: 182 * s, transform: nil))
    ctx.strokePath()

    /* inner hairline frame -- square, engraved into the plate */
    let frame = plate.insetBy(dx: 92 * s, dy: 92 * s)
    if n >= 64 {
        ctx.setStrokeColor(GHOST)
        ctx.setLineWidth(hair)
        ctx.stroke(frame)
    }

    /* registration crosses at the frame corners */
    if n >= 128 {
        let arm = 26 * s
        ctx.setStrokeColor(GHOST)
        ctx.setLineWidth(hair)
        for (px, py) in [(frame.minX, frame.minY), (frame.maxX, frame.minY),
                         (frame.minX, frame.maxY), (frame.maxX, frame.maxY)] {
            ctx.move(to: CGPoint(x: px - arm, y: py)); ctx.addLine(to: CGPoint(x: px + arm, y: py))
            ctx.move(to: CGPoint(x: px, y: py - arm)); ctx.addLine(to: CGPoint(x: px, y: py + arm))
        }
        ctx.strokePath()
    }

    /* the stencil M, ink on black, optically a little above centre */
    let mSize = 560 * s
    let stencil = loadFont("IBMPlexSansCondensed-Bold.ttf", mSize)
    drawCentred(ctx, "M", stencil, INK, 0, cx: CGFloat(n) / 2,
                cy: CGFloat(n) * 0.560)

    /* one blood rule under the M -- the single accent */
    let ruleW = 300 * s, ruleH = max(12 * s, 1)
    let ruleY = CGFloat(n) * 0.268
    ctx.setFillColor(BLOOD)
    ctx.fill(CGRect(x: (CGFloat(n) - ruleW) / 2, y: ruleY, width: ruleW, height: ruleH))
    if n >= 64 { /* armed lamp at the right end of the rule */
        let lamp = max(18 * s, 2)
        ctx.setFillColor(BLOOD_HOT)
        ctx.fill(CGRect(x: (CGFloat(n) + ruleW) / 2 + 14 * s,
                        y: ruleY + ruleH / 2 - lamp / 2, width: lamp, height: lamp))
    }

    /* evidence serial, mono, faint -- large sizes only */
    if n >= 256 {
        let mono = loadFont("IBMPlexMono-Regular.ttf", 42 * s)
        drawCentred(ctx, "N.72-0418", mono, INK_FAINT, 4 * s,
                    cx: CGFloat(n) / 2, cy: CGFloat(n) * 0.208)
    }
    return ctx.makeImage()!
}

let sizes: [(Int, String)] = [
    (16, "icon_16x16.png"), (32, "icon_16x16@2x.png"),
    (32, "icon_32x32.png"), (64, "icon_32x32@2x.png"),
    (128, "icon_128x128.png"), (256, "icon_128x128@2x.png"),
    (256, "icon_256x256.png"), (512, "icon_256x256@2x.png"),
    (512, "icon_512x512.png"), (1024, "icon_512x512@2x.png"),
]
try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
for (n, name) in sizes {
    let img = drawIcon(n)
    let rep = NSBitmapImageRep(cgImage: img)
    rep.size = NSSize(width: n, height: n)
    guard let png = rep.representation(using: .png, properties: [:]) else {
        fputs("png encode failed for \(name)\n", stderr); exit(1)
    }
    try! png.write(to: URL(fileURLWithPath: outDir + "/" + name))
}
print("wrote \(sizes.count) sizes to \(outDir)")
