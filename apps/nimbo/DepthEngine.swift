import Accelerate
import CoreML
import CoreVideo
import Foundation

// AI 3D for the cinema panel: a depth map per video frame from Depth Anything V2 Small (Core ML,
// on the Neural Engine so the GPU keeps the 120 Hz render), handed to the renderer together with
// that very frame. The picture waits for its depth (~25 ms), so a cut never shows the new shot
// with the old shot's depth. Frames that arrive while one is being inferred are dropped except
// the newest, which goes next: the panel shows as many frames as the model keeps up with
// (~40 fps on an M4, every frame of a film).
//
// Thread-safe: submit from the capture queues, route from the main thread.
final class DepthEngine: @unchecked Sendable {
    // One frame and its depth for `panel` (one-component half float, larger = nearer; low / high
    // its 2-98 % percentiles; sceneCut: a new shot, the smoothing starts afresh).
    typealias Output = (_ color: CVPixelBuffer, _ depth: CVPixelBuffer, _ low: Float, _ high: Float,
                        _ sceneCut: Bool, _ panel: Int) -> Void

    struct Stats {
        var frames = 0          // depth maps delivered
        var dropped = 0         // frames replaced by a newer one before their turn
        var cuts = 0
        var scaleMs: Double = 0  // per frame, averaged over the period
        var inferMs: Double = 0
    }

    var onFrame: Output?

    private let lock = NSLock()
    private var model: MLModel?           // guarded by lock
    private var panel = -1                // guarded by lock: the panel whose frames get depth; -1 none
    private var generation = 0            // guarded by lock: bumps on every route change
    private var busy = false              // guarded by lock
    private var pending: (CVPixelBuffer, Int, Int)?  // guarded by lock: frame, panel, generation
    private var stats = Stats()           // guarded by lock

    private let queue = DispatchQueue(label: "nimbo.depth", qos: .userInitiated)
    // Inference queue only.
    private var inputName = "image", outputName = "depth"
    private var inputWidth = 518, inputHeight = 392
    private var inputPool: CVPixelBufferPool?
    private var outputPool: CVPixelBufferPool?
    private var previousThumb: [UInt8] = []
    private var diffAverage: Float = 0
    private var resetScene = true
    private var histogram = [Int](repeating: 0, count: 1024)

    var isLoaded: Bool {
        lock.lock()
        defer { lock.unlock() }
        return model != nil
    }

    // MARK: model

    static let modelName = "DepthAnythingV2SmallF16"
    private static let modelBase = URL(string: "https://huggingface.co/apple/coreml-depth-anything-v2-small/resolve/main/")!
    private static let packageFiles = ["Manifest.json", "Data/com.apple.CoreML/model.mlmodel",
                                       "Data/com.apple.CoreML/weights/weight.bin"]

    static var modelsDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Nimbo/Models", isDirectory: true)
    }

    // The compiled model: in the app bundle, else in Application Support, else downloaded once
    // (48 MB, Apple's Core ML conversion, Apache 2.0) and compiled there.
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
            log("3d: downloading \(modelName) (48 MB)")
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

    // Loads the model (a few seconds the first time: the Neural Engine compiles it). Blocking.
    func load(_ url: URL) throws {
        let config = MLModelConfiguration()
        config.computeUnits = .cpuAndNeuralEngine  // the GPU stays with the render
        let m = try MLModel(contentsOf: url, configuration: config)
        guard let (inName, inDesc) = m.modelDescription.inputDescriptionsByName.first(where: { $0.value.type == .image }),
              let ic = inDesc.imageConstraint,
              let outName = m.modelDescription.outputDescriptionsByName.first(where: { $0.value.type == .image })?.key
        else {
            throw NSError(domain: "DepthEngine", code: 2, userInfo: [NSLocalizedDescriptionKey: "unexpected model inputs"])
        }
        queue.sync {
            inputName = inName
            outputName = outName
            inputWidth = ic.pixelsWide
            inputHeight = ic.pixelsHigh
            inputPool = Self.pool(width: ic.pixelsWide, height: ic.pixelsHigh, format: kCVPixelFormatType_32BGRA)
            outputPool = Self.pool(width: ic.pixelsWide, height: ic.pixelsHigh, format: kCVPixelFormatType_OneComponent16Half)
        }
        lock.lock()
        model = m
        lock.unlock()
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
    // are still in flight are dropped.
    func route(panel p: Int) {
        lock.lock()
        if p != panel {
            panel = p
            generation += 1
            pending = nil
        }
        lock.unlock()
        queue.async { self.resetScene = true }
    }

    var routedPanel: Int {
        lock.lock()
        defer { lock.unlock() }
        return model != nil ? panel : -1
    }

    func takeStats() -> Stats {
        lock.lock()
        defer { lock.unlock() }
        let s = stats
        stats = Stats()
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
        if busy {
            if pending != nil { stats.dropped += 1 }
            pending = (pb, p, g)
            lock.unlock()
            return true
        }
        busy = true
        lock.unlock()
        queue.async { self.run(pb, panel: p, generation: g) }
        return true
    }

    // MARK: inference queue

    private func run(_ first: CVPixelBuffer, panel firstPanel: Int, generation firstGeneration: Int) {
        var job: (CVPixelBuffer, Int, Int)? = (first, firstPanel, firstGeneration)
        while let (pb, p, g) = job {
            autoreleasepool { process(pb, panel: p, generation: g) }
            lock.lock()
            job = pending
            pending = nil
            if job == nil { busy = false }
            lock.unlock()
        }
    }

    private func process(_ color: CVPixelBuffer, panel p: Int, generation g: Int) {
        lock.lock()
        let model = self.model
        lock.unlock()
        guard let model, let inputPool, let outputPool else { return }
        var input: CVPixelBuffer?
        var output: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, inputPool, &input)
        CVPixelBufferPoolCreatePixelBuffer(nil, outputPool, &output)
        guard let input, let output else { return }

        let t0 = DispatchTime.now().uptimeNanoseconds
        guard scale(color, into: input) else { return }
        let cut = sceneCut(input)
        let t1 = DispatchTime.now().uptimeNanoseconds
        do {
            let features = try MLDictionaryFeatureProvider(dictionary: [inputName: MLFeatureValue(pixelBuffer: input)])
            let options = MLPredictionOptions()
            options.outputBackings = [outputName: output]
            let result = try model.prediction(from: features, options: options)
            // Core ML may return its own buffer when it cannot write into ours.
            let depth = result.featureValue(for: outputName)?.imageBufferValue ?? output
            let t2 = DispatchTime.now().uptimeNanoseconds
            let (low, high) = percentiles(depth)
            lock.lock()
            let current = g == generation
            if current {
                stats.frames += 1
                if cut { stats.cuts += 1 }
                stats.scaleMs += Double(t1 - t0) / 1e6
                stats.inferMs += Double(t2 - t1) / 1e6
            }
            lock.unlock()
            if current { onFrame?(color, depth, low, high, cut, p) }
        } catch {
            FileHandle.standardError.write("3d: prediction failed: \(error.localizedDescription)\n".data(using: .utf8)!)
        }
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
        let bins = histogram.count
        for i in 0..<bins { histogram[i] = 0 }
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
