// Minimal interfaces taken from a class dump of CoverSheetKit.framework on
// device (iOS 18.5, build 22F76).
#import <UIKit/UIKit.h>

@interface CSProminentTextElementView : UIView
@property (nonatomic, copy) NSString *overrideString;
- (NSString *)displayString;
@end

@interface CSProminentTimeView : CSProminentTextElementView
- (NSDate *)date;
- (void)setDate:(NSDate *)date;
- (NSString *)_timeString;
@end

@interface CSProminentSubtitleDateView : CSProminentTextElementView
- (NSString *)_dateString;
@end

@interface CSProminentDisplayView : UIView
@property (nonatomic, retain) CSProminentTimeView *timeView;
@property (nonatomic, retain) CSProminentTextElementView *subtitleView;
- (NSDate *)displayDate;
- (void)setDisplayDate:(NSDate *)date;
@end
