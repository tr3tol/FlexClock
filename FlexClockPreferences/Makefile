include $(THEOS)/makefiles/common.mk

BUNDLE_NAME = FlexClockPreferences

FlexClockPreferences_FILES = PrefsBundle.c
FlexClockPreferences_INSTALL_PATH = /Library/PreferenceBundles
# Plain C source, so the ObjC runtime has to be linked explicitly.
FlexClockPreferences_LDFLAGS = -lobjc
FlexClockPreferences_FRAMEWORKS = Foundation CoreFoundation

include $(THEOS_MAKE_PATH)/bundle.mk
