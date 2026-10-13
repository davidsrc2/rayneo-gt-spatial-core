#import "RNVirtualDisplay.h"

// --- Private CoreGraphics interface (runtime-verified on macOS 27.2) -------------------
@interface CGVirtualDisplayDescriptor : NSObject
@property(retain, nonatomic) dispatch_queue_t queue;
@property(retain, nonatomic) NSString* name;
@property(nonatomic) unsigned int maxPixelsHigh;
@property(nonatomic) unsigned int maxPixelsWide;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) unsigned int serialNum;
@property(nonatomic) unsigned int productID;
@property(nonatomic) unsigned int vendorID;
@property(copy, nonatomic) void (^terminationHandler)(id, id);
@end

@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property(retain, nonatomic) NSArray* modes;
@property(nonatomic) unsigned int hiDPI;
@end

@interface CGVirtualDisplay : NSObject
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor*)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings*)settings;
@property(readonly, nonatomic) CGDirectDisplayID displayID;
@end
// ---------------------------------------------------------------------------------------

namespace {

NSError* make_error(NSString* msg) {
    return [NSError errorWithDomain:@"RNVirtualDisplay" code:1 userInfo:@{NSLocalizedDescriptionKey : msg}];
}

}  // namespace

@implementation RNVirtualDisplay {
    CGVirtualDisplay* _display;  // strong: the monitor disappears when released
}

+ (BOOL)isSupported {
    return NSClassFromString(@"CGVirtualDisplay") != nil && NSClassFromString(@"CGVirtualDisplayDescriptor") != nil &&
           NSClassFromString(@"CGVirtualDisplaySettings") != nil && NSClassFromString(@"CGVirtualDisplayMode") != nil;
}

- (nullable instancetype)initWithName:(NSString*)name
                                width:(NSUInteger)width
                               height:(NSUInteger)height
                          refreshRate:(double)refreshRate
                                hiDPI:(BOOL)hiDPI
                               serial:(uint32_t)serial
                                error:(NSError**)error {
    if (!(self = [super init])) return nil;
    if (![RNVirtualDisplay isSupported]) {
        if (error != nullptr) *error = make_error(@"CGVirtualDisplay not available on this macOS");
        return nil;
    }
    const NSUInteger scale = hiDPI ? 2 : 1;

    CGVirtualDisplayDescriptor* d = [[NSClassFromString(@"CGVirtualDisplayDescriptor") alloc] init];
    d.queue = dispatch_get_main_queue();
    d.name = name;
    d.maxPixelsWide = static_cast<unsigned int>(width * scale);
    d.maxPixelsHigh = static_cast<unsigned int>(height * scale);
    // ~110 ppi at 1x: macOS derives the default "looks like" scaling from the physical size.
    d.sizeInMillimeters = CGSizeMake(25.4 * width / 110.0, 25.4 * height / 110.0);
    d.vendorID = 0x524E;  // 'RN'
    d.productID = 0x5344;  // 'SD'
    d.serialNum = serial;
    d.terminationHandler = ^(id, id) {
    };

    _display = [[NSClassFromString(@"CGVirtualDisplay") alloc] initWithDescriptor:d];
    if (_display == nil || _display.displayID == kCGNullDirectDisplay) {
        if (error != nullptr) *error = make_error(@"CGVirtualDisplay creation failed");
        return nil;
    }

    CGVirtualDisplaySettings* s = [[NSClassFromString(@"CGVirtualDisplaySettings") alloc] init];
    s.hiDPI = hiDPI ? 1 : 0;
    // HiDPI modes are declared in points; the backing store is 2x.
    s.modes = @[ [[NSClassFromString(@"CGVirtualDisplayMode") alloc]
        initWithWidth:static_cast<unsigned int>(width * scale)
               height:static_cast<unsigned int>(height * scale)
          refreshRate:refreshRate] ];
    if (![_display applySettings:s]) {
        if (error != nullptr) *error = make_error(@"CGVirtualDisplay applySettings failed");
        return nil;
    }
    _pixelWidth = width * scale;
    _pixelHeight = height * scale;
    _hiDPI = hiDPI;
    return self;
}

- (CGDirectDisplayID)displayID {
    return _display.displayID;
}

@end
