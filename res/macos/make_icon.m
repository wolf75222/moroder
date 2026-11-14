// Build-time tool: draws the Moroder icon (rounded square, gradient, note) as a PNG.
// Usage: make_icon <pixels> <out.png>
#import <AppKit/AppKit.h>

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        const int px = atoi(argv[1]);
        NSString* out = [NSString stringWithUTF8String:argv[2]];

        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL pixelsWide:px pixelsHigh:px bitsPerSample:8 samplesPerPixel:4
            hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];

        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];

        const CGFloat s = px, margin = s * 0.1, radius = s * 0.2;
        NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(margin, margin, s - 2 * margin, s - 2 * margin)
                                                              xRadius:radius yRadius:radius];
        NSGradient* gradient = [[NSGradient alloc]
            initWithStartingColor:[NSColor colorWithSRGBRed:0.95 green:0.15 blue:0.25 alpha:1]
                      endingColor:[NSColor colorWithSRGBRed:0.45 green:0.05 blue:0.35 alpha:1]];
        [gradient drawInBezierPath:shape angle:-60];

        NSDictionary* attrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:s * 0.5 weight:NSFontWeightBold],
            NSForegroundColorAttributeName: NSColor.whiteColor,
        };
        NSAttributedString* note = [[NSAttributedString alloc] initWithString:@"♫" attributes:attrs];
        const NSSize ns = note.size;
        [note drawAtPoint:NSMakePoint((s - ns.width) / 2, (s - ns.height) / 2)];

        [NSGraphicsContext restoreGraphicsState];
        return [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:out atomically:YES] ? 0 : 1;
    }
}
