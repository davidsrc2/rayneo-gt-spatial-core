import Foundation
import simd

struct PoseSample {
    var orientation: simd_quatf      // core frame (X fwd, Y left, Z up), head since recenter
    var angularVelocity: simd_float3 // rad/s, same frame
    var hostTimeNs: UInt64           // RNSpatialEngine.hostNowNs clock
    var tracking: Bool
    var stale = false                // no sample for 0.5 s (stream stalled): held, angular velocity zeroed
    var panelsPitch: Float = 0       // rad, up > 0: the panels lifted to the last recentre's elevation
}

protocol PoseSource: AnyObject {
    var name: String { get }
    func start() throws
    func stop()
    func recenter()
    func latest() -> PoseSample?
}

// The glasses' IMU through the C++ engine.
final class ImuPoseSource: PoseSource {
    let name = "imu"
    private let engine: RNSpatialEngine
    private let calibrationPath: String
    private let calibrationRequired: Bool
    private(set) var calibrationLoaded = false

    init(calibrationPath: String, calibrationRequired: Bool, deadZoneDeg: Float = 0,
         recordPath: String? = nil, replayPath: String? = nil) {
        self.calibrationPath = calibrationPath
        self.calibrationRequired = calibrationRequired
        engine = RNSpatialEngine(deadZoneDegrees: deadZoneDeg)
        engine.setRecordPath(recordPath)
        engine.setReplayPath(replayPath)
    }

    func start() throws {
        do {
            try engine.loadCalibration(fromFile: calibrationPath)
            calibrationLoaded = true
        } catch {
            if calibrationRequired { throw error }
        }
        try engine.start()
    }

    func stop() { engine.stop() }
    func recenter() { engine.recenter() }
    // The glasses' own display mode (0 follow, 1 pinned, 2 steady); pinned again = recentre its anchor.
    func setGlassesMode(_ mode: Int) { engine.setGlassesMode(mode) }
    func setGlassesBrightness(_ level: Int) { engine.setGlassesBrightness(level) }
    var gyroBias: simd_float3? { engine.latestPose()?.gyroBias }
    // Stream stalls so far: stream-on resent / acknowledged / stalls ended, the last one (ms).
    var stalls: (resent: UInt64, acked: UInt64, recovered: UInt64, lastMs: Double) {
        var r: UInt64 = 0, a: UInt64 = 0, v: UInt64 = 0, ms = 0.0
        engine.streamStallsResent(&r, acked: &a, recovered: &v, lastMs: &ms)
        return (r, a, v, ms)
    }
    func simulateStall(ms: Int) { engine.simulateStallMs(ms) }

    func latest() -> PoseSample? {
        guard let p = engine.latestPose() else { return nil }
        return PoseSample(orientation: p.orientation, angularVelocity: p.angularVelocity,
                          hostTimeNs: p.hostTimeNs, tracking: p.isTracking, stale: p.isStale,
                          panelsPitch: p.panelsPitch)
    }
}

// Head simulator for working without the glasses: yaw/pitch driven by hotkeys, or an
// automatic sweep across the panels.
final class SimPoseSource: PoseSource {
    let name = "sim"
    private let lock = NSLock()
    private var yawDeg: Float = 0    // positive = looking left (core +Z)
    private var pitchDeg: Float = 0  // positive = looking up
    private let sweepAmplitudeDeg: Float
    private let sweepPeriodS: Double
    private let t0 = Date()

    init(sweepAmplitudeDeg: Float = 0, sweepPeriodS: Double = 12) {
        self.sweepAmplitudeDeg = sweepAmplitudeDeg
        self.sweepPeriodS = sweepPeriodS
    }

    func start() throws {}
    func stop() {}

    func recenter() {
        lock.lock()
        yawDeg = 0
        pitchDeg = 0
        lock.unlock()
    }

    func nudge(yaw: Float, pitch: Float) {
        lock.lock()
        yawDeg = max(-170, min(170, yawDeg + yaw))
        pitchDeg = max(-80, min(80, pitchDeg + pitch))
        lock.unlock()
    }

    func latest() -> PoseSample? {
        lock.lock()
        var yaw = yawDeg
        let pitch = pitchDeg
        lock.unlock()
        var yawRate: Float = 0
        if sweepAmplitudeDeg > 0 {
            let w = 2 * Double.pi / sweepPeriodS
            let t = Date().timeIntervalSince(t0)
            yaw += sweepAmplitudeDeg * Float(sin(w * t))
            yawRate = sweepAmplitudeDeg * Float(w * cos(w * t)) * .pi / 180
        }
        let d2r = Float.pi / 180
        let qYaw = simd_quatf(angle: yaw * d2r, axis: simd_float3(0, 0, 1))
        let qPitch = simd_quatf(angle: pitch * d2r, axis: simd_float3(0, -1, 0))  // look up = about -Y
        return PoseSample(orientation: qYaw * qPitch, angularVelocity: simd_float3(0, 0, yawRate),
                          hostTimeNs: RNSpatialEngine.hostNowNs(), tracking: true)
    }
}
