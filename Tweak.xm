// FlexClock — image based lock screen clock for iOS 18.
//
// Build environment constraints (Procursus clang-16, arm64e slice):
// the compiler emits code that the iOS 18 runtime rejects for
//   * @"..." / CFSTR() literals   -> SIGSEGV when the literal is retained
//   * compile time ObjC classes   -> crash inside dyld readClass (PtrAuth)
//   * blocks, hence dispatch_once -> SIGSEGV
// Runtime built strings, collections, associated objects, NSDateFormatter,
// CFPreferences and %hook itself are unaffected. SpringBoard loads only the
// arm64e slice, so building for plain arm64 is not an option. Therefore every
// string here comes from S(), there are no custom classes and no blocks.
//
// Lock screen notes (iOS 16+ CoverSheetKit):
//   * timeView lives inside a nested vibrancy container, so its frame has to be
//     translated with convertRect:fromView:
//   * the overlay must be a sibling of that container: BSUIVibrancyEffectView
//     desaturates anything placed inside it
//   * nothing may run from %ctor — touching Foundation or CFPreferences during
//     dylib load hangs SpringBoard, so all setup is lazy, from layoutSubviews
//   * the system clock is blanked by returning an empty _timeString rather than
//     by changing alpha, which would retrigger layout forever

#import <UIKit/UIKit.h>
#import <ImageIO/ImageIO.h>
#import <objc/runtime.h>
#import "PrivateHeaders.h"

// The only safe way to obtain an NSString in this build.
#define S(cstr) ([NSString stringWithUTF8String:(cstr)])

// Packs ship inside the package, imported ones live in a writable directory and
// shadow a bundled pack of the same name.
#define kBuiltinPacks "/var/jb/Library/Application Support/FlexClock/Packs"
#define kUserPacks    "/var/mobile/Library/FlexClock/Packs"
#define kDefaultPack  "Anime.flexpack"
#define kDomain       "com.tr3tol.flexclock"

#pragma mark - Settings

typedef struct {
	BOOL enabled;
	BOOL animated;
	BOOL staticInLowPower;   // drop to still frames while Low Power Mode is on
	BOOL hideDate;
	BOOL colorFilter;
	int format;             // 0 system, 1 12-hour, 2 24-hour
	CGFloat scale;          // digit height, 1.0 == height of the system clock
	CGFloat spacing;        // gap between digits in points, may be negative
	CGFloat verticalOffset; // shifts the whole row, negative moves it up
} FCSettings;

static FCSettings gS;
static NSString *gTintHex = nil;
static NSString *gPackDir = nil;   // resolved folder holding the glyph files
static BOOL gPackOK = NO;          // NO keeps the system clock visible
static BOOL gLoaded = NO;
static BOOL gLowPower = NO;        // state the current pack was resolved for
static int gGeneration = 0;        // bumped on change, forces a redraw

static BOOL FCLowPowerMode(void) {
	return [[NSProcessInfo processInfo] isLowPowerModeEnabled];
}

// Animation is off when the user turned it off, and also while the battery is
// being conserved if they asked for that.
static BOOL FCAnimationWanted(void) {
	if (!gS.animated) return NO;
	return !(gS.staticInLowPower && gLowPower);
}

static CFStringRef FCDomain(void) {
	static CFStringRef domain = NULL;
	if (!domain) domain = CFStringCreateWithCString(NULL, kDomain, kCFStringEncodingUTF8);
	return domain;
}

static id FCPref(const char *key) {
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFPropertyListRef v = CFPreferencesCopyAppValue(k, FCDomain());
	CFRelease(k);
	return v ? (__bridge_transfer id)v : nil;
}

static BOOL FCPrefBool(const char *key, BOOL fallback) {
	id v = FCPref(key);
	return v ? [v boolValue] : fallback;
}

static NSString *FCPrefStr(const char *key, const char *fallback) {
	id v = FCPref(key);
	return [v isKindOfClass:[NSString class]] ? (NSString *)v : S(fallback);
}

static CGFloat FCPrefFloat(const char *key, CGFloat fallback) {
	id v = FCPref(key);
	return v ? (CGFloat)[v doubleValue] : fallback;
}

#pragma mark - Digit packs

// A pack is a folder named <Name>.flexpack holding an "animated" and/or
// "static" subfolder with 0.gif … 9.gif and colon.gif (.png is accepted too).
static BOOL FCFolderHasGlyphs(NSString *dir) {
	NSFileManager *fm = [NSFileManager defaultManager];
	return [fm fileExistsAtPath:[dir stringByAppendingPathComponent:S("0.gif")]] ||
	       [fm fileExistsAtPath:[dir stringByAppendingPathComponent:S("0.png")]];
}

// Falls back to the other animation variant, then to the bundled pack, so a
// missing or malformed pack never leaves the user without a clock.
static void FCResolvePack(void) {
	gPackDir = nil;
	gPackOK = NO;

	NSMutableArray *roots = [NSMutableArray array];
	[roots addObject:S(kUserPacks)];
	[roots addObject:S(kBuiltinPacks)];

	NSMutableArray *packs = [NSMutableArray array];
	[packs addObject:FCPrefStr("pack", kDefaultPack)];
	[packs addObject:S(kDefaultPack)];

	BOOL animate = FCAnimationWanted();
	NSMutableArray *variants = [NSMutableArray array];
	[variants addObject:(animate ? S("animated") : S("static"))];
	[variants addObject:(animate ? S("static") : S("animated"))];

	for (NSString *pack in packs) {
		for (NSString *root in roots) {
			for (NSString *variant in variants) {
				NSString *dir = [[root stringByAppendingPathComponent:pack]
				                        stringByAppendingPathComponent:variant];
				if (FCFolderHasGlyphs(dir)) {
					gPackDir = dir;
					gPackOK = YES;
					return;
				}
			}
		}
	}
}

#pragma mark - Image cache

// NSCache so the decoded frames are dropped automatically under memory
// pressure instead of pinning several megabytes for the whole SpringBoard life.
// The limit is in bytes rather than entries: a glyph costs whatever its frames
// decode to, and a pack with large artwork would otherwise grow without bound.
static NSCache *FCCache(void) {
	static NSCache *cache = nil;
	if (!cache) {
		cache = [[NSCache alloc] init];
		cache.totalCostLimit = 12 * 1024 * 1024;
	}
	return cache;
}

static NSUInteger FCImageCost(UIImage *image) {
	NSUInteger frames = image.images.count > 0 ? image.images.count : 1;
	CGFloat scale = image.scale > 0 ? image.scale : 1.0;
	return (NSUInteger)(image.size.width * scale * image.size.height * scale * 4.0) * frames;
}

static UIColor *FCColorFromHex(NSString *hex) {
	if (hex.length == 0) return [UIColor systemOrangeColor];
	unsigned int rgb = 0;
	NSScanner *sc = [NSScanner scannerWithString:
		[hex stringByReplacingOccurrencesOfString:S("#") withString:S("")]];
	if (![sc scanHexInt:&rgb]) return [UIColor systemOrangeColor];
	return [UIColor colorWithRed:((rgb & 0xFF0000) >> 16) / 255.0
	                       green:((rgb & 0x00FF00) >> 8) / 255.0
	                        blue:(rgb & 0x0000FF) / 255.0
	                       alpha:1.0];
}

static UIImage *FCDecodeImage(NSString *path) {
	if (![[NSFileManager defaultManager] fileExistsAtPath:path]) return nil;

	CGImageSourceRef src = CGImageSourceCreateWithURL(
		(__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
	if (!src) return nil;

	size_t count = CGImageSourceGetCount(src);
	if (count == 0) { CFRelease(src); return nil; }

	NSMutableArray *frames = [NSMutableArray array];
	NSTimeInterval total = 0;

	for (size_t i = 0; i < count; i++) {
		CGImageRef cg = CGImageSourceCreateImageAtIndex(src, i, NULL);
		if (!cg) continue;
		[frames addObject:[UIImage imageWithCGImage:cg]];
		CGImageRelease(cg);

		NSDictionary *props = (__bridge_transfer NSDictionary *)
			CGImageSourceCopyPropertiesAtIndex(src, i, NULL);
		NSDictionary *gif = props[(__bridge NSString *)kCGImagePropertyGIFDictionary];
		NSNumber *delay = gif[(__bridge NSString *)kCGImagePropertyGIFUnclampedDelayTime];
		if (!delay) delay = gif[(__bridge NSString *)kCGImagePropertyGIFDelayTime];
		total += delay ? [delay doubleValue] : 0.1;
	}
	CFRelease(src);

	if (frames.count == 0) return nil;
	if (frames.count == 1) return frames[0];
	return [UIImage animatedImageWithImages:frames
	                               duration:(total > 0 ? total : frames.count * 0.1)];
}

// Recolours a frame the way iOS tints app icons: hue and saturation come from
// the chosen colour, brightness stays with the artwork, so shading survives.
// imageWithTintColor: cannot be used, it flattens the sprite into a silhouette.
static UIImage *FCTintFrame(UIImage *src, UIColor *color) {
	CGSize size = src.size;
	CGImageRef cg = src.CGImage;
	if (size.width <= 0 || size.height <= 0 || !cg) return src;

	UIGraphicsBeginImageContextWithOptions(size, NO, src.scale);
	CGContextRef ctx = UIGraphicsGetCurrentContext();
	if (!ctx) { UIGraphicsEndImageContext(); return src; }

	CGRect rect = CGRectMake(0, 0, size.width, size.height);

	// Core Graphics has its origin at the bottom, flip so image and mask align.
	CGContextTranslateCTM(ctx, 0, size.height);
	CGContextScaleCTM(ctx, 1.0, -1.0);

	CGContextDrawImage(ctx, rect, cg);

	// Without clipping to the alpha channel the fill also covers the empty
	// areas and the sprite ends up inside a coloured rectangle.
	CGContextClipToMask(ctx, rect, cg);

	CGContextSetBlendMode(ctx, kCGBlendModeColor);
	CGContextSetFillColorWithColor(ctx, color.CGColor);
	CGContextFillRect(ctx, rect);

	UIImage *out = UIGraphicsGetImageFromCurrentImageContext();
	UIGraphicsEndImageContext();
	return out ? out : src;
}

static UIImage *FCTintImage(UIImage *src, UIColor *color) {
	NSArray *frames = src.images;
	if (frames.count == 0) return FCTintFrame(src, color);

	NSMutableArray *tinted = [NSMutableArray arrayWithCapacity:frames.count];
	for (UIImage *frame in frames) [tinted addObject:FCTintFrame(frame, color)];
	return [UIImage animatedImageWithImages:tinted duration:src.duration];
}

static UIImage *FCGlyph(NSString *glyph) {
	if (!gPackOK) return nil;

	NSString *key = [NSString stringWithFormat:S("%@|%@|%d|%@"),
	                 gPackDir, glyph, gS.colorFilter ? 1 : 0,
	                 gTintHex ? gTintHex : S("")];

	UIImage *cached = [FCCache() objectForKey:key];
	if (cached) return cached;

	UIImage *image = FCDecodeImage([NSString stringWithFormat:S("%@/%@.gif"), gPackDir, glyph]);
	if (!image) image = FCDecodeImage([NSString stringWithFormat:S("%@/%@.png"), gPackDir, glyph]);
	if (!image) return nil;

	if (gS.colorFilter) image = FCTintImage(image, FCColorFromHex(gTintHex));

	[FCCache() setObject:image forKey:key cost:FCImageCost(image)];
	return image;
}

#pragma mark - Time

static NSString *FCTimeString(NSDate *date) {
	static NSDateFormatter *formatter = nil;
	static int builtForFormat = -1;

	// Creating a formatter pulls in ICU and the locale, so it is rebuilt only
	// when the selected format actually changes.
	if (!formatter || builtForFormat != gS.format) {
		const char *tpl = (gS.format == 1) ? "hmm" : (gS.format == 2) ? "HHmm" : "jmm";

		// Force latin digits: locales such as Arabic or Hindi would otherwise
		// produce characters that have no matching glyph in the pack.
		NSString *identifier = [[NSLocale currentLocale] localeIdentifier];
		identifier = [identifier stringByAppendingString:
			([identifier rangeOfString:S("@")].location == NSNotFound)
				? S("@numbers=latn") : S(";numbers=latn")];
		NSLocale *locale = [NSLocale localeWithLocaleIdentifier:identifier];

		formatter = [[NSDateFormatter alloc] init];
		formatter.locale = locale;
		formatter.dateFormat = [NSDateFormatter dateFormatFromTemplate:S(tpl)
		                                                        options:0
		                                                         locale:locale];
		builtForFormat = gS.format;
	}
	return [formatter stringFromDate:date];
}

#pragma mark - Settings loading

static void FCLoadSettings(void) {
	gS.enabled = FCPrefBool("enabled", YES);
	gS.animated = [FCPrefStr("animationStyle", "animated") isEqualToString:S("animated")];
	gS.staticInLowPower = FCPrefBool("staticInLowPower", NO);

	gS.scale = FCPrefFloat("clockScale", 1.0);
	if (gS.scale < 0.1) gS.scale = 0.1;
	if (gS.scale > 4.0) gS.scale = 4.0;

	gS.spacing = FCPrefFloat("digitSpacing", -2.0);
	gS.verticalOffset = FCPrefFloat("verticalOffset", 0.0);

	NSString *format = FCPrefStr("timeFormat", "system");
	gS.format = [format isEqualToString:S("12hour")] ? 1
	          : [format isEqualToString:S("24hour")] ? 2 : 0;

	gS.hideDate = FCPrefBool("hideDate", NO);
	gS.colorFilter = FCPrefBool("colorFilterEnabled", NO);
	gTintHex = FCPrefStr("tintColor", "#FF9500");

	gLowPower = FCLowPowerMode();
	FCResolvePack();
}

static void FCPrefsChanged(CFNotificationCenterRef c, void *o, CFStringRef n,
                           const void *object, CFDictionaryRef info) {
	FCLoadSettings();
	// Cache keys embed the pack and tint, so stale entries would never be hit
	// again and would only keep memory alive.
	[FCCache() removeAllObjects];
	gGeneration++;
}

// The Settings app is not allowed to restart SpringBoard, so it posts a signal
// and the respring happens here, inside SpringBoard itself.
static void FCRespringRequested(CFNotificationCenterRef c, void *o, CFStringRef n,
                                const void *object, CFDictionaryRef info) {
	exit(0); // launchd brings SpringBoard back up
}

// Lazy init without dispatch_once, which relies on blocks. Safe because this is
// only reached from layoutSubviews, i.e. always on the main thread.
static void FCEnsureLoaded(void) {
	if (gLoaded) return;
	gLoaded = YES;
	FCLoadSettings();

	CFNotificationCenterRef center = CFNotificationCenterGetDarwinNotifyCenter();

	CFStringRef changed = CFStringCreateWithCString(NULL, kDomain "/preferenceschanged",
	                                                kCFStringEncodingUTF8);
	CFNotificationCenterAddObserver(center, NULL, FCPrefsChanged, changed, NULL,
	                                CFNotificationSuspensionBehaviorDeliverImmediately);

	CFStringRef respring = CFStringCreateWithCString(NULL, kDomain "/respring",
	                                                 kCFStringEncodingUTF8);
	CFNotificationCenterAddObserver(center, NULL, FCRespringRequested, respring, NULL,
	                                CFNotificationSuspensionBehaviorDeliverImmediately);
	// The names are intentionally not released: they live as long as the observers.
}

#pragma mark - Overlay

static void *kOverlay = &kOverlay;
static void *kLastMinute = &kLastMinute;
static void *kLastSize = &kLastSize;
static void *kLastGeneration = &kLastGeneration;

// Polled rather than observed: an NSNotificationCenter observer needs either a
// block or a custom class, and neither is available in this build. This runs
// from both hooks, so the state is picked up on the next redraw — at the latest
// when the minute changes.
static void FCSyncPowerState(void) {
	// layoutSubviews can fire many times a second during transitions, and the
	// power state changes at most a handful of times a day, so sampling it is
	// rate limited rather than done on every pass.
	static CFAbsoluteTime lastSample = 0;
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (now - lastSample < 1.0) return;
	lastSample = now;

	BOOL low = FCLowPowerMode();
	if (low == gLowPower) return;

	// Tracked even when the option is off, otherwise the stored state goes
	// stale and the next comparison is made against the wrong value.
	gLowPower = low;
	if (!gS.staticInLowPower) return;

	FCResolvePack();
	gGeneration++;
}

static void FCRender(UIView *overlay, NSDate *date) {
	FCSyncPowerState();
	if (!gPackOK) return;

	CGSize size = overlay.bounds.size;
	if (size.height <= 0 || size.width <= 0) return;

	// layoutSubviews fires far more often than the clock changes. Comparing the
	// minute first avoids running the date formatter on every pass.
	long minute = (long)floor([date timeIntervalSinceReferenceDate] / 60.0);
	NSNumber *lastMinute = objc_getAssociatedObject(overlay, kLastMinute);
	NSValue *lastSize = objc_getAssociatedObject(overlay, kLastSize);
	NSNumber *lastGeneration = objc_getAssociatedObject(overlay, kLastGeneration);
	if (lastMinute && [lastMinute longValue] == minute &&
	    lastSize && CGSizeEqualToSize([lastSize CGSizeValue], size) &&
	    lastGeneration && [lastGeneration intValue] == gGeneration) {
		return;
	}

	NSString *time = FCTimeString(date);

	NSMutableArray *images = [NSMutableArray array];
	NSMutableArray *widths = [NSMutableArray array];
	CGFloat digitHeight = size.height * gS.scale;
	CGFloat totalWidth = 0;

	for (NSUInteger i = 0; i < time.length; i++) {
		unichar ch = [time characterAtIndex:i];
		NSString *glyph = nil;
		if (ch >= '0' && ch <= '9') {
			char buf[2] = { (char)ch, 0 };
			glyph = S(buf);
		} else if (ch == ':') {
			glyph = S("colon");
		}
		if (!glyph) continue;

		UIImage *image = FCGlyph(glyph);
		if (!image) continue;

		CGFloat aspect = image.size.height > 0 ? image.size.width / image.size.height : 0.6;
		CGFloat width = digitHeight * aspect;
		[images addObject:image];
		[widths addObject:[NSNumber numberWithDouble:width]];
		totalWidth += width;
	}
	if (images.count == 0) return;
	totalWidth += gS.spacing * (images.count - 1);

	// Image views are pooled: recreating them on every redraw would churn the
	// view hierarchy once a minute for as long as the device is up.
	NSMutableArray *pool = [NSMutableArray array];
	for (UIView *sub in overlay.subviews) {
		if ([sub isKindOfClass:[UIImageView class]]) [pool addObject:sub];
	}
	while (pool.count < images.count) {
		UIImageView *view = [[UIImageView alloc] init];
		view.contentMode = UIViewContentModeScaleAspectFit;
		[overlay addSubview:view];
		[pool addObject:view];
	}
	for (NSUInteger i = images.count; i < pool.count; i++) {
		UIImageView *view = pool[i];
		view.hidden = YES;
		view.image = nil; // an unused view must not keep an animation alive
	}

	CGFloat x = (size.width - totalWidth) / 2.0;
	CGFloat y = (size.height - digitHeight) / 2.0 + gS.verticalOffset;
	for (NSUInteger i = 0; i < images.count; i++) {
		UIImageView *view = pool[i];
		CGFloat width = [widths[i] doubleValue];
		view.hidden = NO;
		if (view.image != images[i]) view.image = images[i];
		view.frame = CGRectMake(x, y, width, digitHeight);
		x += width + gS.spacing;
	}

	objc_setAssociatedObject(overlay, kLastMinute, [NSNumber numberWithLong:minute],
	                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	objc_setAssociatedObject(overlay, kLastSize, [NSValue valueWithCGSize:size],
	                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	objc_setAssociatedObject(overlay, kLastGeneration, [NSNumber numberWithInt:gGeneration],
	                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

#pragma mark - Hooks

%hook CSProminentDisplayView

- (void)layoutSubviews {
	%orig;

	FCEnsureLoaded();

	UIView *timeView = (UIView *)self.timeView;
	if (!timeView) return;

	UIView *overlay = objc_getAssociatedObject(self, kOverlay);

	if (!gS.enabled || !gPackOK) {
		if (overlay && !overlay.hidden) {
			overlay.hidden = YES;
			// Drop the images so hidden views stop holding decoded frames.
			for (UIView *sub in overlay.subviews) {
				if ([sub isKindOfClass:[UIImageView class]]) ((UIImageView *)sub).image = nil;
			}
			objc_setAssociatedObject(overlay, kLastMinute, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		}
		return;
	}

	// Created lazily so it survives the lock screen being rebuilt, which the
	// system does every time the display turns on.
	if (!overlay) {
		overlay = [[UIView alloc] initWithFrame:CGRectZero];
		overlay.userInteractionEnabled = NO;
		objc_setAssociatedObject(self, kOverlay, overlay, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	if (overlay.superview != self) [self addSubview:overlay];
	overlay.hidden = NO;

	CGRect target = [self convertRect:timeView.bounds fromView:timeView];
	if (!CGRectEqualToRect(overlay.frame, target)) overlay.frame = target;

	NSDate *date = self.displayDate;
	FCRender(overlay, date ? date : [NSDate date]);
}

- (void)setDisplayDate:(NSDate *)date {
	%orig;

	if (!gLoaded || !gS.enabled || !gPackOK) return;
	UIView *overlay = objc_getAssociatedObject(self, kOverlay);
	if (!overlay || overlay.hidden) return;

	FCRender(overlay, date ? date : [NSDate date]);
}

%end

%hook CSProminentTimeView

- (NSString *)_timeString {
	if (gLoaded && gS.enabled && gPackOK) return S("");
	return %orig;
}

%end

%hook CSProminentSubtitleDateView

- (NSString *)_dateString {
	if (gLoaded && gS.enabled && gPackOK && gS.hideDate) return S("");
	return %orig;
}

%end
