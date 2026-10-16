import Accelerate
import CoreML
import CoreVideo
import Foundation

// AI 3D for the cinema panel: a depth map per video frame from Depth Anything V2 Small (Core ML),
// handed to the renderer together with that very frame. The picture waits for its depth, so a cut
// never shows the new shot with the old shot's depth.
//
// Throughput (M4, 8-bit palettised weights): the Neural Engine alone runs one inference in ~22 ms,
// two in flight overlap their CPU work and give ~53 fps. When the video brings more frames than
// that (a 60 fps stream), a GPU copy of the model takes a third slot: ~75 fps together. A film
// (24 fps) never needs it, so then the GPU is all the render's. Results can finish out of order
// (the GPU is slower): each waits for the ones before it, so every inferred frame is shown, in order.
//
// Thread-safe: submit from the capture queues, route and stats from the main thread.
final class DepthEngine: @unchecked Sendable {
    // One frame and its depth for `panel` (one-component half float, larger = nearer; low / high
    // its 2-98 % percentiles; sceneCut: a new shot, the smoothing starts afresh).
    typealias Output = (_ color: CVPixelBuffer, _ depth: CVPixelBuffer, _ low: Float, _ high: Float,
                        _ sceneCut: Bool, _ panel: Int) -> Void

    struct Stats {
        var frames = 0          // depth maps delivered
        var dropped = 0         // frames replaced by a newer one before a slot was free
        var waitMs: Double = 0  // summed: time finished frames waited for an earlier one
        var cuts = 0
        var gpuFrames = 0       // of `frames`, inferred on the GPU
        var scaleMs: Double = 0  // per frame, summed over the period
        var inferMs: Double = 0
    }

    var onFrame: Output?
    var allowGPU = true  // set before loading: the GPU may help when the Neural Engine falls behind

    private enum Slot { case neuralEngine, gpu }
    private static let neuralEngineSlots = 2

    private let lock = NSLock()
    private var model: MLModel?           // guarded by lock: Neural Engine
    private var gpuModel: MLModel?        // guarded by lock: loaded once frames start dropping
    private var gpuLoading = false        // guarded by lock
    private var modelURL: URL?            // guarded by lock
    private var panel = -1                // guarded by lock: the panel whose frames get depth; -1 none
    private var generation = 0            // guarded by lock: bumps on every route change
    private var neuralEngineBusy = 0      // guarded by lock: inferences in flight there
    private var gpuBusy = false           // guarded by lock
    private var pending: (CVPixelBuffer, Int, Int)?  // guarded by lock: frame, panel, generation
    private var lastDelivered = -1        // guarded by lock: sequence number of the last frame shown
    private var cutFrames = Set<Int>()    // guarded by lock: sequence numbers of frames that start a shot
    private var carryCut = false          // guarded by lock: a cut in a frame that was never shown
    // Guarded by lock: finished frames waiting for an earlier one (nil result: failed or stale).
    private var finished: [Int: (result: Result?, generation: Int, slot: Slot, at: UInt64)] = [:]
    private typealias Result = (depth: CVPixelBuffer, low: Float, high: Float, color: CVPixelBuffer, panel: Int,
                                scaleMs: Double, inferMs: Double)
    private var droppingSeconds = 0       // guarded by lock: consecutive stats periods with drops
    private var stats = Stats()           // guarded by lock

    // Preparation queue (serial, in arrival order): scaling, scene cuts, sequence numbers.
    private let prepQueue = DispatchQueue(label: "nimbo.depth.prep", qos: .userInitiated)
    private let inferQueue = DispatchQueue(label: "nimbo.depth.infer", qos: .userInitiated, attributes: .concurrent)
    private var inputName = "image", outputName = "depth"  // set at load, then read only
    private var inputPool: CVPixelBufferPool?
    private var outputPool: CVPixelBufferPool?
    private var nextSequence = 0          // prep queue
    private var previousThumb: [UInt8] = []  // prep queue
    private var diffAverage: Float = 0    // prep queue
    private var resetScene = true         // prep queue

    var isLoaded: Bool {
        lock.lock()
        defer { lock.unlock() }
        return model != nil
    }

    // MARK: model

    static let modelName = "DepthAnythingV2SmallF16P8"  // 8-bit palettised: as good, ~10 % faster, half the size
    private static let modelBase = URL(string: "https://huggingface.co/apple/coreml-depth-anything-v2-small/resolve/main/")!
    private static let packageFiles = ["Manifest.json", "Data/com.apple.CoreML/model.mlmodel",
                                       "Data/com.apple.CoreML/weights/weight.bin"]

    static var modelsDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Nimbo/Models", isDirectory: true)
    }

    // The compiled model: in the app bundle, else in Application Support, else downloaded once
    // (25 MB, Apple's Core ML conversion, Apache 2.0) and compiled there.
    static func compiledModel(log: @escaping (String) -> Void) async throws -> URL {
        if let bundled = Bundle.main.url(forResource: modelName, withExtension: "mlmodelc") { return bundled }
        let fm = FileManager.default
        let dir = modelsDirectory
        let compiled = dir.appendingPathComponent("\(modelName).mlmodelc")
        if fm.fileExists(atPath: compiled.path) { return compiled }
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        var package = Bundle.main.url(forResource: modelName, withExtension: "mlpackage")
        if package == nil {
            let target = dir.appendingPathComponent("\(modelName).mlpackage")
            let partial = dir.appendingPathComponent("\(modelName).download")
            try? fm.removeItem(at: partial)
            log("3d: downloading \(modelName) (25 MB)")
            for file in packageFiles {
                let url = modelBase.appendingPathComponent("\(modelName).mlpackage/\(file)")
                let (tmp, response) = try await URLSession.shared.download(from: url)
                guard (response as? HTTPURLResponse)?.statusCode == 200 else {
                    throw NSError(domain: "DepthEngine", code: 1,
                                  userInfo: [NSLocalizedDescriptionKey: "download failed: \(file)"])
                }
                let dest = partial.appendingPathComponent(file)
                try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
                try fm.moveItem(at: tmp, to: dest)
            }
            try? fm.removeItem(at: target)
            try fm.moveItem(at: partial, to: target)
            package = target
        }
        log("3d: compiling the model")
        let tmp = try await MLModel.compileModel(at: package!)
        try? fm.removeItem(at: compiled)
        try fm.moveItem(at: tmp, to: compiled)
        if package!.deletingLastPathComponent() == dir { try? fm.removeItem(at: package!) }  // only the compiled one is used
        return compiled
    }

    // Loads the model on the Neural Engine (a few seconds the first time: it compiles it there). Blocking.
    func load(_ url: URL) throws {
        let m = try Self.model(url, units: .cpuAndNeuralEngine)
        guard let (inName, inDesc) = m.modelDescription.inputDescriptionsByName.first(where: { $0.value.type == .image }),
              let ic = inDesc.imageConstraint,
              let outName = m.modelDescription.outputDescriptionsByName.first(where: { $0.value.type == .image })?.key
        else {
            throw NSError(domain: "DepthEngine", code: 2, userInfo: [NSLocalizedDescriptionKey: "unexpected model inputs"])
        }
        prepQueue.sync {
            inputName = inName
            outputName = outName
            inputPool = Self.pool(width: ic.pixelsWide, height: ic.pixelsHigh, format: kCVPixelFormatType_32BGRA)
            outputPool = Self.pool(width: ic.pixelsWide, height: ic.pixelsHigh, format: kCVPixelFormatType_OneComponent16Half)
        }
        lock.lock()
        model = m
        modelURL = url
        lock.unlock()
    }

    private static func model(_ url: URL, units: MLComputeUnits) throws -> MLModel {
        let config = MLModelConfiguration()
        config.computeUnits = units
        return try MLModel(contentsOf: url, configuration: config)
    }

    // The Neural Engine has been falling behind: a GPU copy of the model takes the extra frames.
    private func loadGPUModel(_ url: URL) {
        DispatchQueue.global(qos: .utility).async {
            let m = try? Self.model(url, units: .cpuAndGPU)
            self.lock.lock()
            self.gpuModel = m
            self.gpuLoading = false
            self.lock.unlock()
            FileHandle.standardError.write("3d: GPU helps the Neural Engine: \(m != nil ? "on" : "failed")\n".data(using: .utf8)!)
        }
    }

    private static func pool(width: Int, height: Int, format: OSType) -> CVPixelBufferPool? {
        let attrs: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: format,
            kCVPixelBufferWidthKey: width,
            kCVPixelBufferHeightKey: height,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
            kCVPixelBufferMetalCompatibilityKey: true,
        ]
        var pool: CVPixelBufferPool?
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
        return pool
    }

    // MARK: routing

    // The panel whose frames get depth from now on (-1: none). Frames of the panel before that
    // still in flight are dropped.
    func route(panel p: Int) {
        lock.lock()
        if p != panel {
            panel = p
            generation += 1
            pending = nil
        }
        lock.unlock()
        prepQueue.async { self.resetScene = true }
    }

    var routedPanel: Int {
        lock.lock()
        defer { lock.unlock() }
        return model != nil ? panel : -1
    }

    var gpuActive: Bool {
        lock.lock()
        defer { lock.unlock() }
        return gpuModel != nil
    }

    // The last period's numbers (call once a second). Two seconds in a row of dropped frames
    // bring in the GPU.
    func takeStats() -> Stats {
        lock.lock()
        let s = stats
        stats = Stats()
        droppingSeconds = s.dropped > 2 ? droppingSeconds + 1 : 0
        var url: URL?
        if droppingSeconds >= 2, allowGPU, gpuModel == nil, !gpuLoading, let u = modelURL {
            gpuLoading = true
            url = u
        }
        lock.unlock()
        if let url { loadGPUModel(url) }
        return s
    }

    // A captured frame. false: not this engine's panel (or the model is not ready), show it as is.
    func submit(_ pb: CVPixelBuffer, panel p: Int) -> Bool {
        lock.lock()
        guard model != nil, p == panel, p >= 0 else {
            lock.unlock()
            return false
        }
        let g = generation
        if let slot = takeSlot() {
            lock.unlock()
            start(pb, panel: p, generation: g, slot: slot)
        } else {
            if pending != nil { stats.dropped += 1 }
            pending = (pb, p, g)
            lock.unlock()
        }
        return true
    }

    // Under lock.
    private func takeSlot() -> Slot? {
        if neuralEngineBusy < Self.neuralEngineSlots {
            neuralEngineBusy += 1
            return .neuralEngine
        }
        if gpuModel != nil && !gpuBusy {
            gpuBusy = true
            return .gpu
        }
        return nil
    }

    // MARK: pipeline

    private func start(_ color: CVPixelBuffer, panel p: Int, generation g: Int, slot: Slot) {
        prepQueue.async {
            let seq = self.nextSequence
            self.nextSequence += 1
            var input: CVPixelBuffer?
            if let pool = self.inputPool { CVPixelBufferPoolCreatePixelBuffer(nil, pool, &input) }
            let t0 = DispatchTime.now().uptimeNanoseconds
            guard let input, self.scale(color, into: input) else {
                self.finish(nil, seq: seq, generation: g, slot: slot)
                return
            }
            if self.sceneCut(input) {
                self.lock.lock()
                self.cutFrames.insert(seq)
                self.lock.unlock()
            }
            let scaleMs = Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6
            self.inferQueue.async {
                autoreleasepool {
                    let depth = self.infer(input, slot: slot)
                    self.finish(depth.map { Result($0.0, $0.1, $0.2, color, p, scaleMs, $0.3) }, seq: seq, generation: g, slot: slot)
                }
            }
        }
    }

    // (depth, low, high, inference ms)
    private func infer(_ input: CVPixelBuffer, slot: Slot) -> (CVPixelBuffer, Float, Float, Double)? {
        lock.lock()
        let m = slot == .gpu ? gpuModel : model
        lock.unlock()
        var output: CVPixelBuffer?
        if let pool = outputPool { CVPixelBufferPoolCreatePixelBuffer(nil, pool, &output) }
        guard let m, let output else { return nil }
        let t0 = DispatchTime.now().uptimeNanoseconds
        do {
            let features = try MLDictionaryFeatureProvider(dictionary: [inputName: MLFeatureValue(pixelBuffer: input)])
            let options = MLPredictionOptions()
            options.outputBackings = [outputName: output]
            let result = try m.prediction(from: features, options: options)
            // Core ML may return its own buffer when it cannot write into ours.
            let depth = result.featureValue(for: outputName)?.imageBufferValue ?? output
            let ms = Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6
            let (low, high) = percentiles(depth)
            return (depth, low, high, ms)
        } catch {
            FileHandle.standardError.write("3d: prediction failed: \(error.localizedDescription)\n".data(using: .utf8)!)
            return nil
        }
    }

    // A slot is free again: the frame and every finished one after it in sequence are shown
    // (unless a route change made them stale), and the waiting frame (if any) takes the slot.
    private func finish(_ r: Result?, seq: Int, generation g: Int, slot: Slot) {
        lock.lock()
        switch slot {
        case .neuralEngine: neuralEngineBusy -= 1
        case .gpu: gpuBusy = false
        }
        let now = DispatchTime.now().uptimeNanoseconds
        finished[seq] = (r, g, slot, now)
        // Under the lock: deliveries stay in sequence order whichever thread makes them.
        while let f = finished.removeValue(forKey: lastDelivered + 1) {
            lastDelivered += 1
            let cut = carryCut || cutFrames.remove(lastDelivered) != nil
            guard let r = f.result, f.generation == generation else {
                carryCut = cut
                continue
            }
            carryCut = false
            stats.frames += 1
            if cut { stats.cuts += 1 }
            if f.slot == .gpu { stats.gpuFrames += 1 }
            stats.scaleMs += r.scaleMs
            stats.inferMs += r.inferMs
            stats.waitMs += Double(now - f.at) / 1e6
            onFrame?(r.color, r.depth, r.low, r.high, cut, r.panel)
        }
        var next: ((CVPixelBuffer, Int, Int), Slot)?
        if let job = pending, let s = takeSlot() {
            pending = nil
            next = (job, s)
        }
        lock.unlock()
        if let ((pb, p, g2), s) = next { start(pb, panel: p, generation: g2, slot: s) }
    }

    // The frame shrunk (and stretched) to the model's input: the depth map is stretched back
    // over the panel, so every point keeps its place.
    private func scale(_ src: CVPixelBuffer, into dst: CVPixelBuffer) -> Bool {
        guard CVPixelBufferGetPixelFormatType(src) == kCVPixelFormatType_32BGRA else { return false }
        CVPixelBufferLockBaseAddress(src, .readOnly)
        CVPixelBufferLockBaseAddress(dst, [])
        defer {
            CVPixelBufferUnlockBaseAddress(dst, [])
            CVPixelBufferUnlockBaseAddress(src, .readOnly)
        }
        guard let sBase = CVPixelBufferGetBaseAddress(src), let dBase = CVPixelBufferGetBaseAddress(dst) else { return false }
        var s = vImage_Buffer(data: sBase, height: vImagePixelCount(CVPixelBufferGetHeight(src)),
                              width: vImagePixelCount(CVPixelBufferGetWidth(src)), rowBytes: CVPixelBufferGetBytesPerRow(src))
        var d = vImage_Buffer(data: dBase, height: vImagePixelCount(CVPixelBufferGetHeight(dst)),
                              width: vImagePixelCount(CVPixelBufferGetWidth(dst)), rowBytes: CVPixelBufferGetBytesPerRow(dst))
        return vImageScale_ARGB8888(&s, &d, nil, vImage_Flags(kvImageNoFlags)) == kvImageNoError
    }

    // A new shot: the brightness of a coarse grid over the shrunk frame changes a lot from the
    // frame before, or much more than it has been changing (a fast pan changes it steadily).
    private func sceneCut(_ frame: CVPixelBuffer) -> Bool {
        let gw = 48, gh = 36
        CVPixelBufferLockBaseAddress(frame, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(frame, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(frame) else { return false }
        let w = CVPixelBufferGetWidth(frame), h = CVPixelBufferGetHeight(frame), bpr = CVPixelBufferGetBytesPerRow(frame)
        var thumb = [UInt8](repeating: 0, count: gw * gh)
        for gy in 0..<gh {
            let row = base.advanced(by: (gy * h / gh + h / (2 * gh)) * bpr).assumingMemoryBound(to: UInt8.self)
            for gx in 0..<gw {
                let px = row.advanced(by: (gx * w / gw + w / (2 * gw)) * 4)  // BGRA
                thumb[gy * gw + gx] = UInt8((Int(px[2]) * 54 + Int(px[1]) * 183 + Int(px[0]) * 19) >> 8)
            }
        }
        defer { previousThumb = thumb }
        if resetScene || previousThumb.count != thumb.count {
            resetScene = false
            diffAverage = 0
            return true
        }
        var sum = 0
        for i in 0..<thumb.count { sum += abs(Int(thumb[i]) - Int(previousThumb[i])) }
        let diff = Float(sum) / Float(thumb.count * 255)
        let cut = diff > 0.2 || (diff > 0.08 && diff > 4 * diffAverage)
        if !cut { diffAverage += (diff - diffAverage) * 0.1 }
        return cut
    }

    // 2 % and 98 % of the depth values: a few extreme pixels do not squash the rest.
    private func percentiles(_ depth: CVPixelBuffer) -> (Float, Float) {
        CVPixelBufferLockBaseAddress(depth, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(depth, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(depth) else { return (0, 1) }
        let w = CVPixelBufferGetWidth(depth), h = CVPixelBufferGetHeight(depth), bpr = CVPixelBufferGetBytesPerRow(depth)
        var lo = Float.infinity, hi = -Float.infinity
        for y in 0..<h {
            let row = base.advanced(by: y * bpr).assumingMemoryBound(to: Float16.self)
            for x in 0..<w {
                let v = Float(row[x])
                lo = min(lo, v)
                hi = max(hi, v)
            }
        }
        guard hi > lo else { return (lo.isFinite ? lo : 0, lo.isFinite ? lo + 1 : 1) }
        let bins = 1024
        var histogram = [Int](repeating: 0, count: bins)
        let k = Float(bins - 1) / (hi - lo)
        for y in 0..<h {
            let row = base.advanced(by: y * bpr).assumingMemoryBound(to: Float16.self)
            for x in 0..<w { histogram[Int((Float(row[x]) - lo) * k)] += 1 }
        }
        let total = w * h
        var acc = 0, p2 = 0, p98 = bins - 1
        var found2 = false
        for i in 0..<bins {
            acc += histogram[i]
            if !found2 && acc >= total * 2 / 100 { p2 = i; found2 = true }
            if acc >= total * 98 / 100 { p98 = i; break }
        }
        let low = lo + Float(p2) / k, high = lo + Float(p98 + 1) / k
        return (low, max(high, low + 1e-3))
    }
}
