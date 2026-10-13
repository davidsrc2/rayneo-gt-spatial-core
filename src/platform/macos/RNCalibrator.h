// Guided head calibration for the macOS app: runs rayneo::platform::GuidedCalibration on a
// background thread; the UI polls snapshots.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, RNCalibStep) {
    RNCalibStepBias = 0,
    RNCalibStepCenter,
    RNCalibStepUp,
    RNCalibStepDown,
    RNCalibStepLeft,
    RNCalibStepRight,
};

@interface RNCalibrationSnapshot : NSObject
@property(nonatomic, readonly) RNCalibStep step;
@property(nonatomic, readonly) BOOL returning;     // direction captured, back to centre
@property(nonatomic, readonly) NSString* hint;     // move / further / hold / waiting for rest ...
@property(nonatomic, readonly) float rateDegrees;  // head rate, deg/s
@property(nonatomic, readonly) float rotationDegrees;
@property(nonatomic, readonly) BOOL biasOK;
@property(nonatomic, readonly) BOOL running;
@property(nonatomic, readonly) BOOL finished;
@property(nonatomic, readonly) BOOL succeeded;
@property(nonatomic, readonly) NSString* message;  // error or result summary when finished
@end

@interface RNCalibrator : NSObject
// Writes calibration.json to `path` on success. No-op if already running.
- (void)startWritingTo:(NSString*)path;
- (void)cancel;
- (RNCalibrationSnapshot*)snapshot;
@end

NS_ASSUME_NONNULL_END
