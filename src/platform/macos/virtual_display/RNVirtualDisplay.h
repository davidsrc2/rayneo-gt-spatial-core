// Virtual monitors for the spatial desktop. Wraps CoreGraphics' private CGVirtualDisplay
// (same API used by DeskPad / BetterDisplay); the private types stay in the .mm.
// A virtual display exists while this object is alive.

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface RNVirtualDisplay : NSObject
@property(nonatomic, readonly) CGDirectDisplayID displayID;
@property(nonatomic, readonly) NSUInteger pixelWidth;   // backing pixels
@property(nonatomic, readonly) NSUInteger pixelHeight;
@property(nonatomic, readonly) BOOL hiDPI;

// width/height in points; hiDPI doubles the backing store (sharper text, 4x capture cost).
- (nullable instancetype)initWithName:(NSString*)name
                                width:(NSUInteger)width
                               height:(NSUInteger)height
                          refreshRate:(double)refreshRate
                                hiDPI:(BOOL)hiDPI
                               serial:(uint32_t)serial
                                error:(NSError* _Nullable* _Nullable)error;

+ (BOOL)isSupported;  // private classes present at runtime
@end

NS_ASSUME_NONNULL_END
