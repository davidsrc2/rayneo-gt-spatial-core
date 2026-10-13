#import "RNCalibrator.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>

#include "rayneo/core/head_calibration.h"
#include "rayneo/platform/guided_calibration.h"

@interface RNCalibrationSnapshot ()
@property(nonatomic) RNCalibStep step;
@property(nonatomic) BOOL returning;
@property(nonatomic, copy) NSString* hint;
@property(nonatomic) float rateDegrees;
@property(nonatomic) float rotationDegrees;
@property(nonatomic) BOOL biasOK;
@property(nonatomic) BOOL running;
@property(nonatomic) BOOL finished;
@property(nonatomic) BOOL succeeded;
@property(nonatomic, copy) NSString* message;
@end

@implementation RNCalibrationSnapshot
@end

@implementation RNCalibrator {
    std::mutex _mutex;
    rayneo::platform::CalibStatus _status;
    std::string _hint;
    bool _running;
    bool _finished;
    bool _succeeded;
    std::string _message;
    std::atomic<bool> _cancel;
    std::thread _thread;
}

- (void)dealloc {
    _cancel.store(true);
    if (_thread.joinable()) _thread.join();
}

- (void)startWritingTo:(NSString*)path {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_running) return;
        _running = true;
        _finished = false;
        _succeeded = false;
        _message.clear();
        _status = {};
    }
    if (_thread.joinable()) _thread.join();
    _cancel.store(false);
    const std::string out = path.fileSystemRepresentation;
    _thread = std::thread([self, out] {
        const auto outcome = rayneo::platform::GuidedCalibration().run(
            _cancel, [self](const rayneo::platform::CalibStatus& st) {
                std::lock_guard<std::mutex> lock(_mutex);
                _status = st;
                _hint = st.hint;
            });
        bool ok = false;
        std::string msg;
        if (!outcome.completed) {
            msg = outcome.error;
        } else if (!outcome.result.ok) {
            msg = outcome.result.error;
        } else {
            std::ofstream f(out);
            f << rayneo::core::calibration_to_json(outcome.result.sensor_to_head);
            ok = static_cast<bool>(f);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "yaw %.1f°, nod %.1f°, mismatch %.1f°", outcome.result.yaw_axis_error_deg,
                          outcome.result.pitch_axis_tilt_deg, outcome.result.nod_mismatch_deg);
            msg = ok ? buf : "cannot write " + out;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _running = false;
        _finished = true;
        _succeeded = ok;
        _message = msg;
    });
}

- (void)cancel {
    _cancel.store(true);
}

- (RNCalibrationSnapshot*)snapshot {
    RNCalibrationSnapshot* s = [RNCalibrationSnapshot new];
    std::lock_guard<std::mutex> lock(_mutex);
    s.step = static_cast<RNCalibStep>(_status.step);
    s.returning = _status.returning;
    s.hint = [NSString stringWithUTF8String:_hint.c_str()];
    s.rateDegrees = _status.rate_degs;
    s.rotationDegrees = _status.rotation_deg;
    s.biasOK = _status.bias_ok;
    s.running = _running;
    s.finished = _finished;
    s.succeeded = _succeeded;
    s.message = [NSString stringWithUTF8String:_message.c_str()];
    return s;
}

@end
