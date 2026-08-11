// FlexClock preferences bundle.
//
// iOS refuses to load a preference bundle that has no executable, and a plain
// PSListController does not read Root.plist by itself, so a controller class is
// required. The controllers are assembled through the ObjC runtime instead of
// normal @interface declarations because this toolchain corrupts compile time
// class metadata in the arm64e slice — the full analysis is in Tweak.xm.
//
// The same limitation rules out blocks, so there are no UIAlertController
// confirmations here: destructive actions live on their own page instead.

#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <string.h>

#define kBundlePath     "/var/jb/Library/PreferenceBundles/FlexClockPreferences.bundle"
#define kBuiltinPacks   "/var/jb/Library/Application Support/FlexClock/Packs"
#define kUserPacks      "/var/mobile/Library/FlexClock/Packs"
#define kDomain         "com.tr3tol.flexclock"
#define kPackExtension  ".flexpack"

// PSCellType values used below.
#define kGroupCell  0
#define kLinkCell   1
#define kValueCell  4
#define kButtonCell 13

static void *kSpecifiersKey = &kSpecifiersKey;
static void *kRevisionKey = &kRevisionKey;
static void *kDelegateKey = &kDelegateKey;
static void *kOwnerKey = &kOwnerKey;

// Bumped whenever a pack is added or removed so open pages rebuild themselves.
static int gPacksRevision = 1;

// Outcome of the last import or deletion, shown under the pack list so a
// refusal states its reason. Deliberately kept in memory only: persisting it
// would leave a stale message sitting in the settings forever.
static id gLastStatus = NULL;

#pragma mark - Runtime helpers

static id FCStr(const char *utf8) {
	return ((id (*)(id, SEL, const char *))objc_msgSend)(
		(id)objc_getClass("NSString"), sel_getUid("stringWithUTF8String:"), utf8);
}

static id FCSend(id target, const char *selector) {
	return ((id (*)(id, SEL))objc_msgSend)(target, sel_getUid(selector));
}

static id FCSend1(id target, const char *selector, id arg) {
	return ((id (*)(id, SEL, id))objc_msgSend)(target, sel_getUid(selector), arg);
}

static void FCSend2v(id target, const char *selector, id a, id b) {
	((void (*)(id, SEL, id, id))objc_msgSend)(target, sel_getUid(selector), a, b);
}

static long FCCount(id collection) {
	return collection ? ((long (*)(id, SEL))objc_msgSend)(collection, sel_getUid("count")) : 0;
}

static id FCAt(id array, long index) {
	return ((id (*)(id, SEL, long))objc_msgSend)(array, sel_getUid("objectAtIndex:"), index);
}

static id FCArray(void) {
	return FCSend((id)objc_getClass("NSMutableArray"), "array");
}

static BOOL FCHasSuffix(id string, const char *suffix) {
	if (!string) return NO;
	return ((BOOL (*)(id, SEL, id))objc_msgSend)(string, sel_getUid("hasSuffix:"), FCStr(suffix));
}

static BOOL FCIsEqual(id string, id other) {
	if (!string || !other) return NO;
	return ((BOOL (*)(id, SEL, id))objc_msgSend)(string, sel_getUid("isEqualToString:"), other);
}

static id FCPathAppend(id base, id component) {
	return FCSend1(base, "stringByAppendingPathComponent:", component);
}

static id FCFileManager(void) {
	return FCSend((id)objc_getClass("NSFileManager"), "defaultManager");
}

static BOOL FCExists(id path) {
	return ((BOOL (*)(id, SEL, id))objc_msgSend)(
		FCFileManager(), sel_getUid("fileExistsAtPath:"), path);
}

static id FCDirectoryContents(const char *path) {
	return ((id (*)(id, SEL, id, void *))objc_msgSend)(
		FCFileManager(), sel_getUid("contentsOfDirectoryAtPath:error:"), FCStr(path), NULL);
}

static CFStringRef FCCFStr(const char *utf8) {
	return CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8);
}

static void FCPostChanged(void) {
	CFStringRef name = FCCFStr(kDomain "/preferenceschanged");
	CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
	                                     name, NULL, NULL, TRUE);
	CFRelease(name);
}

static void FCSetPref(const char *key, id value) {
	CFStringRef domain = FCCFStr(kDomain);
	CFStringRef k = FCCFStr(key);
	CFPreferencesSetAppValue(k, (CFPropertyListRef)value, domain);
	CFPreferencesAppSynchronize(domain);
	CFRelease(k);
	CFRelease(domain);
	FCPostChanged();
}

#pragma mark - Pack discovery

// A pack is a folder called <Name>.flexpack. User packs live in a writable
// directory and shadow a built in pack of the same name, so a bundled pack can
// be replaced without touching the installed package.
static id FCPackInfo(id folder, id name, BOOL user) {
	id info = FCSend((id)objc_getClass("NSMutableDictionary"), "dictionary");

	id title = NULL, version = NULL;
	id plist = ((id (*)(id, SEL, id))objc_msgSend)(
		(id)objc_getClass("NSDictionary"), sel_getUid("dictionaryWithContentsOfFile:"),
		FCPathAppend(folder, FCStr("Info.plist")));
	if (plist) {
		title = FCSend1(plist, "objectForKey:", FCStr("Name"));
		version = FCSend1(plist, "objectForKey:", FCStr("Version"));
	}
	if (!title) title = FCSend(name, "stringByDeletingPathExtension");

	FCSend2v(info, "setObject:forKey:", name, FCStr("id"));
	FCSend2v(info, "setObject:forKey:", folder, FCStr("path"));
	FCSend2v(info, "setObject:forKey:", title, FCStr("title"));
	if (version) FCSend2v(info, "setObject:forKey:", version, FCStr("version"));
	FCSend2v(info, "setObject:forKey:",
	         ((id (*)(id, SEL, BOOL))objc_msgSend)((id)objc_getClass("NSNumber"),
	                                               sel_getUid("numberWithBool:"), user),
	         FCStr("user"));
	return info;
}

static void FCCollectPacks(id list, const char *root, BOOL user) {
	id entries = FCDirectoryContents(root);
	if (!entries) return;

	entries = ((id (*)(id, SEL, SEL))objc_msgSend)(
		entries, sel_getUid("sortedArrayUsingSelector:"), sel_getUid("localizedStandardCompare:"));

	long total = FCCount(entries);
	for (long i = 0; i < total; i++) {
		id name = FCAt(entries, i);
		if (!FCHasSuffix(name, kPackExtension)) continue;

		// A user pack of the same name wins, so skip the built in duplicate.
		BOOL duplicate = NO;
		long known = FCCount(list);
		for (long j = 0; j < known; j++) {
			if (FCIsEqual(FCSend1(FCAt(list, j), "objectForKey:", FCStr("id")), name)) {
				duplicate = YES;
				break;
			}
		}
		if (duplicate) continue;

		id folder = FCPathAppend(FCStr(root), name);
		FCSend1(list, "addObject:", FCPackInfo(folder, name, user));
	}
}

static id FCAllPacks(void) {
	id list = FCArray();
	FCCollectPacks(list, kUserPacks, YES);
	FCCollectPacks(list, kBuiltinPacks, NO);
	return list;
}

#pragma mark - Specifier construction

static id FCSpecifierWithGetter(id target, id name, long cellType, SEL getter) {
	return ((id (*)(id, SEL, id, id, SEL, SEL, Class, long, Class))objc_msgSend)(
		(id)objc_getClass("PSSpecifier"),
		sel_getUid("preferenceSpecifierNamed:target:set:get:detail:cell:edit:"),
		name, target, NULL, getter, NULL, cellType, NULL);
}

static id FCSpecifier(id target, id name, long cellType) {
	return FCSpecifierWithGetter(target, name, cellType, NULL);
}

// A value row has no preference behind it, so the getter just hands back the
// text stored on the specifier. Without a getter the cell renders empty.
static id FCReadSpecifierValue(id self, SEL _cmd, id specifier) {
	return FCSend1(specifier, "propertyForKey:", FCStr("fcValue"));
}

static void FCSetSpecifierProperty(id specifier, id value, const char *key) {
	FCSend2v(specifier, "setProperty:forKey:", value, FCStr(key));
}

static id FCGroup(id target, id label, id footer) {
	id group = FCSpecifier(target, label, kGroupCell);
	if (footer) FCSetSpecifierProperty(group, footer, "footerText");
	return group;
}

static id FCButton(id target, id label, const char *action) {
	id specifier = FCSpecifier(target, label, kButtonCell);
	((void (*)(id, SEL, SEL))objc_msgSend)(specifier, sel_getUid("setButtonAction:"),
	                                       sel_getUid(action));
	return specifier;
}

static id FCValueRow(id target, id label, id value) {
	id specifier = FCSpecifierWithGetter(target, label, kValueCell,
	                                     sel_getUid("fcReadSpecifierValue:"));
	FCSetSpecifierProperty(specifier, value, "fcValue");
	return specifier;
}

#pragma mark - Shared page plumbing

// PSListController reads the private _specifiers ivar directly and clears it
// when the page is left, so it has to be written back on every call — otherwise
// rows go missing after navigating away and coming back.
static void FCPublish(id self, id specifiers) {
	Ivar ivar = class_getInstanceVariable(objc_getClass("PSListController"), "_specifiers");
	if (ivar && object_getIvar(self, ivar) != specifiers) {
		FCSend(specifiers, "retain");
		object_setIvar(self, ivar, specifiers);
	}
}

static id FCCached(id self) {
	id cached = objc_getAssociatedObject(self, kSpecifiersKey);
	if (!cached) return NULL;

	id revision = objc_getAssociatedObject(self, kRevisionKey);
	if (revision && ((int (*)(id, SEL))objc_msgSend)(revision, sel_getUid("intValue")) == gPacksRevision) {
		return cached;
	}
	return NULL;
}

static id FCStore(id self, id specifiers) {
	objc_setAssociatedObject(self, kSpecifiersKey, specifiers, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	objc_setAssociatedObject(self, kRevisionKey,
	                         ((id (*)(id, SEL, int))objc_msgSend)((id)objc_getClass("NSNumber"),
	                                                              sel_getUid("numberWithInt:"),
	                                                              gPacksRevision),
	                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	FCPublish(self, specifiers);
	return specifiers;
}

static id FCLoadPlistPage(id self, const char *plistName) {
	id bundle = ((id (*)(id, SEL, id))objc_msgSend)(
		(id)objc_getClass("NSBundle"), sel_getUid("bundleWithPath:"), FCStr(kBundlePath));

	return ((id (*)(id, SEL, id, id, id))objc_msgSend)(
		self, sel_getUid("loadSpecifiersFromPlistName:target:bundle:"),
		FCStr(plistName), self, bundle);
}

static void FCInvalidate(id page) {
	objc_setAssociatedObject(page, kSpecifiersKey, NULL, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	Ivar ivar = class_getInstanceVariable(objc_getClass("PSListController"), "_specifiers");
	if (ivar) object_setIvar(page, ivar, NULL);
	FCSend(page, "reloadSpecifiers");
}

// Leaves the pack list behind rather than trying to refresh it in place. The
// popped controller is released, so walking back into it builds the list from
// disk again. Refreshing from a viewWillAppear: override is not an option:
// reloading specifiers there makes the view appear again and recurses until the
// stack runs out.
static void FCPopPastPackList(id self) {
	id navigation = FCSend(self, "navigationController");
	id stack = FCSend(navigation, "viewControllers");

	for (long i = 1; i < FCCount(stack); i++) {
		if (strcmp(object_getClassName(FCAt(stack, i)), "FlexClockPacksListController") != 0) continue;
		((void (*)(id, SEL, id, BOOL))objc_msgSend)(
			navigation, sel_getUid("popToViewController:animated:"), FCAt(stack, i - 1), YES);
		return;
	}
	FCSend1(navigation, "popViewControllerAnimated:", (id)(long)YES);
}

#pragma mark - Root page

// Keeps the inline pack picker in Root.plist in sync with what is installed.
// The plist already lists the bundled packs, and the scan only adds to it:
// replacing the list outright means anything that stops the directory being
// read leaves the picker holding fewer packs than are actually installed.
static void FCFillPackPicker(id specifiers) {
	for (long i = 0; i < FCCount(specifiers); i++) {
		id specifier = FCAt(specifiers, i);
		if (!FCIsEqual(FCSend1(specifier, "propertyForKey:", FCStr("key")), FCStr("pack")))
			continue;

		id values = FCSend1(specifier, "propertyForKey:", FCStr("validValues"));
		id titles = FCSend1(specifier, "propertyForKey:", FCStr("validTitles"));
		values = values ? FCSend1((id)objc_getClass("NSMutableArray"), "arrayWithArray:", values)
		                : FCArray();
		titles = titles ? FCSend1((id)objc_getClass("NSMutableArray"), "arrayWithArray:", titles)
		                : FCArray();

		id packs = FCAllPacks();
		for (long j = 0; j < FCCount(packs); j++) {
			id pack = FCAt(packs, j);
			id name = FCSend1(pack, "objectForKey:", FCStr("id"));

			BOOL known = NO;
			for (long n = 0; n < FCCount(values); n++) {
				if (FCIsEqual(FCAt(values, n), name)) { known = YES; break; }
			}
			if (known) continue;

			FCSend1(values, "addObject:", name);
			FCSend1(titles, "addObject:", FCSend1(pack, "objectForKey:", FCStr("title")));
		}

		FCSetSpecifierProperty(specifier, values, "validValues");
		FCSetSpecifierProperty(specifier, titles, "validTitles");
		break;
	}
}

static id FCRootSpecifiers(id self, SEL _cmd) {
	id cached = FCCached(self);
	if (cached) { FCPublish(self, cached); return cached; }

	id specifiers = FCLoadPlistPage(self, "Root");
	if (!specifiers) return NULL;
	FCFillPackPicker(specifiers);
	return FCStore(self, specifiers);
}

static id FCAboutSpecifiers(id self, SEL _cmd) {
	id cached = FCCached(self);
	if (cached) { FCPublish(self, cached); return cached; }

	id specifiers = FCLoadPlistPage(self, "About");
	if (!specifiers) return NULL;
	return FCStore(self, specifiers);
}

#pragma mark - Pack list page

static id FCPacksSpecifiers(id self, SEL _cmd) {
	id cached = FCCached(self);
	if (cached) { FCPublish(self, cached); return cached; }

	id specifiers = FCArray();
	id packs = FCAllPacks();

	FCSend1(specifiers, "addObject:", FCGroup(self, FCStr("INSTALLED"), NULL));

	Class detail = objc_getClass("FlexClockPackListController");
	for (long i = 0; i < FCCount(packs); i++) {
		id pack = FCAt(packs, i);
		id row = FCSpecifier(self, FCSend1(pack, "objectForKey:", FCStr("title")), kLinkCell);
		((void (*)(id, SEL, Class))objc_msgSend)(row, sel_getUid("setDetailControllerClass:"), detail);
		FCSetSpecifierProperty(row, pack, "fcPack");
		FCSend1(specifiers, "addObject:", row);
	}

	id footer = gLastStatus ? gLastStatus : FCStr("Choose a .flexpack folder.");
	FCSend1(specifiers, "addObject:", FCGroup(self, FCStr("IMPORT"), footer));
	FCSend1(specifiers, "addObject:", FCButton(self, FCStr("Import pack…"), "fcImportPack"));

	return FCStore(self, specifiers);
}

#pragma mark - Single pack page

static id FCCurrentPack(id self) {
	id specifier = FCSend(self, "specifier");
	return specifier ? FCSend1(specifier, "propertyForKey:", FCStr("fcPack")) : NULL;
}

static id FCPackSpecifiers(id self, SEL _cmd) {
	id cached = FCCached(self);
	if (cached) { FCPublish(self, cached); return cached; }

	id pack = FCCurrentPack(self);
	id specifiers = FCArray();
	if (!pack) return FCStore(self, specifiers);

	id version = FCSend1(pack, "objectForKey:", FCStr("version"));

	FCSend1(specifiers, "addObject:", FCGroup(self, NULL, NULL));
	if (version) {
		FCSend1(specifiers, "addObject:", FCValueRow(self, FCStr("Version"), version));
	}
	FCSend1(specifiers, "addObject:", FCButton(self, FCStr("Use this pack"), "fcUsePack"));

	id user = FCSend1(pack, "objectForKey:", FCStr("user"));
	if (user && ((BOOL (*)(id, SEL))objc_msgSend)(user, sel_getUid("boolValue"))) {
		FCSend1(specifiers, "addObject:", FCGroup(self, NULL, NULL));
		id remove = FCButton(self, FCStr("Delete pack"), "fcDeletePack");
		FCSetSpecifierProperty(remove, ((id (*)(id, SEL, BOOL))objc_msgSend)(
			(id)objc_getClass("NSNumber"), sel_getUid("numberWithBool:"), YES), "isDestructive");
		FCSend1(specifiers, "addObject:", remove);
	} else {
		FCSend1(specifiers, "addObject:",
		        FCGroup(self, NULL, FCStr("Bundled packs are part of the installed package "
		                                  "and cannot be deleted here.")));
	}

	return FCStore(self, specifiers);
}

static void FCUsePack(id self, SEL _cmd) {
	id pack = FCCurrentPack(self);
	if (!pack) return;
	FCSetPref("pack", FCSend1(pack, "objectForKey:", FCStr("id")));
	FCSend1(FCSend(self, "navigationController"), "popViewControllerAnimated:", (id)(long)YES);
}

static void FCRecordStatus(const char *stage, id detail) {
	id text = FCStr(stage);
	if (detail) text = FCSend1(text, "stringByAppendingString:", detail);

	if (gLastStatus) FCSend(gLastStatus, "release");
	gLastStatus = FCSend(text, "retain");
}

static void FCDeletePack(id self, SEL _cmd) {
	id pack = FCCurrentPack(self);
	if (!pack) { FCRecordStatus("delete: no pack on specifier", NULL); return; }

	id user = FCSend1(pack, "objectForKey:", FCStr("user"));
	if (!user || !((BOOL (*)(id, SEL))objc_msgSend)(user, sel_getUid("boolValue"))) {
		FCRecordStatus("delete: refused, pack is bundled", NULL);
		return;
	}

	id path = FCSend1(pack, "objectForKey:", FCStr("path"));
	id error = NULL;
	BOOL removed = ((BOOL (*)(id, SEL, id, id *))objc_msgSend)(
		FCFileManager(), sel_getUid("removeItemAtPath:error:"), path, &error);

	if (!removed) {
		FCRecordStatus("delete FAILED: ", error ? FCSend(error, "localizedDescription") : path);
		return;
	}
	FCRecordStatus("delete ok: ", path);

	gPacksRevision++;
	FCPostChanged();
	FCPopPastPackList(self);
}

#pragma mark - Import

static BOOL FCIsDirectory(id path) {
	BOOL directory = NO;
	((BOOL (*)(id, SEL, id, BOOL *))objc_msgSend)(
		FCFileManager(), sel_getUid("fileExistsAtPath:isDirectory:"), path, &directory);
	return directory;
}

static void FCEnsureUserDirectory(void) {
	((BOOL (*)(id, SEL, id, BOOL, id, void *))objc_msgSend)(
		FCFileManager(), sel_getUid("createDirectoryAtPath:withIntermediateDirectories:attributes:error:"),
		FCStr(kUserPacks), YES, NULL, NULL);
}

static BOOL FCVariantHasGlyphs(id folder, const char *variant) {
	id dir = FCPathAppend(folder, FCStr(variant));
	return FCExists(FCPathAppend(dir, FCStr("0.gif"))) ||
	       FCExists(FCPathAppend(dir, FCStr("0.png")));
}

static BOOL FCLooksLikePack(id folder) {
	return FCVariantHasGlyphs(folder, "animated") || FCVariantHasGlyphs(folder, "static");
}

// The picker hands back whatever the user tapped, which may be a parent folder
// rather than the pack itself. Look one level down before giving up.
static id FCResolvePickedPack(id source) {
	if (FCLooksLikePack(source)) return source;

	id entries = ((id (*)(id, SEL, id, void *))objc_msgSend)(
		FCFileManager(), sel_getUid("contentsOfDirectoryAtPath:error:"), source, NULL);
	for (long i = 0; i < FCCount(entries); i++) {
		id child = FCPathAppend(source, FCAt(entries, i));
		if (FCIsDirectory(child) && FCLooksLikePack(child)) return child;
	}
	return NULL;
}

// Copies a picked .flexpack folder into the writable pack directory.
static BOOL FCInstallPackFromPath(id picked) {
	if (!picked || !FCIsDirectory(picked)) {
		FCRecordStatus("import rejected: not a folder", NULL);
		return NO;
	}

	id source = FCResolvePickedPack(picked);
	if (!source) {
		FCRecordStatus("import rejected, no animated or static digits in: ", picked);
		return NO;
	}

	id name = FCSend(source, "lastPathComponent");
	if (!FCHasSuffix(name, kPackExtension)) {
		name = FCSend1(FCSend(name, "stringByDeletingPathExtension"),
		               "stringByAppendingString:", FCStr(kPackExtension));
	}

	FCEnsureUserDirectory();
	id destination = FCPathAppend(FCStr(kUserPacks), name);

	if (FCExists(destination)) {
		((BOOL (*)(id, SEL, id, void *))objc_msgSend)(
			FCFileManager(), sel_getUid("removeItemAtPath:error:"), destination, NULL);
	}

	id error = NULL;
	BOOL copied = ((BOOL (*)(id, SEL, id, id, id *))objc_msgSend)(
		FCFileManager(), sel_getUid("copyItemAtPath:toPath:error:"), source, destination, &error);
	if (copied) {
		FCRecordStatus("import ok: ", destination);
		gPacksRevision++;
		FCPostChanged();
	} else {
		FCRecordStatus("import FAILED: ", error ? FCSend(error, "localizedDescription") : source);
	}
	return copied;
}

// UIDocumentPickerViewController delegate. It has to be a real object, so it is
// another runtime built class; the picker keeps it alive through an associated
// object because the delegate property is weak.
static void FCPickerDidPick(id self, SEL _cmd, id picker, id urls) {
	for (long i = 0; i < FCCount(urls); i++) {
		id url = FCAt(urls, i);
		BOOL scoped = ((BOOL (*)(id, SEL))objc_msgSend)(
			url, sel_getUid("startAccessingSecurityScopedResource"));
			FCInstallPackFromPath(FCSend(url, "path"));
		if (scoped) FCSend(url, "stopAccessingSecurityScopedResource");
	}

	// The picker was presented by the list page, which is what needs redrawing.
	id presenting = objc_getAssociatedObject(picker, kOwnerKey);
	if (presenting) FCInvalidate(presenting);
}

static BOOL FCInstancesRespond(Class cls, SEL selector) {
	return ((BOOL (*)(id, SEL, SEL))objc_msgSend)(
		(id)cls, sel_getUid("instancesRespondToSelector:"), selector);
}

static void FCImportPack(id self, SEL _cmd) {
	Class pickerClass = objc_getClass("UIDocumentPickerViewController");
	if (!pickerClass) return;

	id picker = NULL;

	// iOS 14 replaced the document type API. Both are probed rather than assumed:
	// messaging a selector that no longer exists would take the app down.
	SEL modern = sel_getUid("initForOpeningContentTypes:");
	Class utType = objc_getClass("UTType");
	if (utType && FCInstancesRespond(pickerClass, modern)) {
		id folder = ((id (*)(id, SEL, id))objc_msgSend)(
			(id)utType, sel_getUid("typeWithIdentifier:"), FCStr("public.folder"));
		if (folder) {
			id types = FCArray();
			FCSend1(types, "addObject:", folder);
			picker = ((id (*)(id, SEL, id))objc_msgSend)(
				FCSend((id)pickerClass, "alloc"), modern, types);
		}
	}

	SEL legacy = sel_getUid("initWithDocumentTypes:inMode:");
	if (!picker && FCInstancesRespond(pickerClass, legacy)) {
		id types = FCArray();
		FCSend1(types, "addObject:", FCStr("public.folder"));
		picker = ((id (*)(id, SEL, id, long))objc_msgSend)(
			FCSend((id)pickerClass, "alloc"), legacy, types, 1 /* Import */);
	}
	if (!picker) return;

	Class delegateClass = objc_getClass("FlexClockPickerDelegate");
	id delegate = delegateClass ? FCSend(FCSend((id)delegateClass, "alloc"), "init") : NULL;
	if (delegate) {
		FCSend1(picker, "setDelegate:", delegate);
		objc_setAssociatedObject(picker, kDelegateKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	objc_setAssociatedObject(picker, kOwnerKey, self, OBJC_ASSOCIATION_ASSIGN);

	((void (*)(id, SEL, id, BOOL, id))objc_msgSend)(
		self, sel_getUid("presentViewController:animated:completion:"), picker, YES, NULL);
}

#pragma mark - Root page actions

static const char *kResetKeys[] = {
	"enabled", "pack", "animationStyle", "staticInLowPower", "clockScale",
	"digitSpacing", "verticalOffset", "timeFormat", "hideDate",
	"colorFilterEnabled", "tintColor"
};

static void FCResetDefaults(id self, SEL _cmd) {
	CFStringRef domain = FCCFStr(kDomain);
	for (unsigned i = 0; i < sizeof(kResetKeys) / sizeof(kResetKeys[0]); i++) {
		CFStringRef key = FCCFStr(kResetKeys[i]);
		CFPreferencesSetAppValue(key, NULL, domain); // NULL removes the key
		CFRelease(key);
	}
	CFPreferencesAppSynchronize(domain);
	CFRelease(domain);
	FCPostChanged();

	// Rebuild the page so every control shows its default again.
	objc_setAssociatedObject(self, kSpecifiersKey, NULL, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	Ivar ivar = class_getInstanceVariable(objc_getClass("PSListController"), "_specifiers");
	if (ivar) object_setIvar(self, ivar, NULL);
	FCSend(self, "reloadSpecifiers");
}

static void FCRespring(id self, SEL _cmd) {
	CFStringRef name = FCCFStr(kDomain "/respring");
	CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
	                                     name, NULL, NULL, TRUE);
	CFRelease(name);
}

static void FCOpen(const char *url) {
	Class application = objc_getClass("UIApplication");
	if (!application) return;
	id shared = FCSend((id)application, "sharedApplication");
	if (!shared) return;

	id target = ((id (*)(id, SEL, id))objc_msgSend)(
		(id)objc_getClass("NSURL"), sel_getUid("URLWithString:"), FCStr(url));
	if (!target) return;

	((void (*)(id, SEL, id, id, id))objc_msgSend)(
		shared, sel_getUid("openURL:options:completionHandler:"), target, NULL, NULL);
}

static void FCOpenDeveloper(id self, SEL _cmd) { FCOpen("https://github.com/tr3tol"); }
static void FCOpenCredits(id self, SEL _cmd)   { FCOpen("https://github.com/yandevelop"); }

#pragma mark - Registration

static void FCRegisterPage(const char *name, IMP specifiers) {
	if (objc_getClass(name)) return;

	Class base = objc_getClass("PSListController");
	if (!base) return;

	Class page = objc_allocateClassPair(base, name, 0);
	if (!page) return;

	class_addMethod(page, sel_getUid("specifiers"), specifiers, "@@:");
	class_addMethod(page, sel_getUid("fcReadSpecifierValue:"), (IMP)FCReadSpecifierValue, "@@:@");
	class_addMethod(page, sel_getUid("fcResetDefaults"), (IMP)FCResetDefaults, "v@:");
	class_addMethod(page, sel_getUid("fcRespring"), (IMP)FCRespring, "v@:");
	class_addMethod(page, sel_getUid("fcOpenDeveloper"), (IMP)FCOpenDeveloper, "v@:");
	class_addMethod(page, sel_getUid("fcOpenCredits"), (IMP)FCOpenCredits, "v@:");
	class_addMethod(page, sel_getUid("fcImportPack"), (IMP)FCImportPack, "v@:");
	class_addMethod(page, sel_getUid("fcUsePack"), (IMP)FCUsePack, "v@:");
	class_addMethod(page, sel_getUid("fcDeletePack"), (IMP)FCDeletePack, "v@:");
	objc_registerClassPair(page);
}

static void FCRegisterPickerDelegate(void) {
	if (objc_getClass("FlexClockPickerDelegate")) return;

	Class base = objc_getClass("NSObject");
	if (!base) return;

	Class delegate = objc_allocateClassPair(base, "FlexClockPickerDelegate", 0);
	if (!delegate) return;

	class_addMethod(delegate, sel_getUid("documentPicker:didPickDocumentsAtURLs:"),
	                (IMP)FCPickerDidPick, "v@:@@");
	objc_registerClassPair(delegate);
}

__attribute__((constructor))
static void FCRegisterControllers(void) {
	FCRegisterPage("FlexClockRootListController", (IMP)FCRootSpecifiers);
	FCRegisterPage("FlexClockAboutListController", (IMP)FCAboutSpecifiers);
	FCRegisterPage("FlexClockPacksListController", (IMP)FCPacksSpecifiers);
	FCRegisterPage("FlexClockPackListController", (IMP)FCPackSpecifiers);
	FCRegisterPickerDelegate();
}
