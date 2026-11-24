import AppKit
import EverythingCore
import Foundation
import UniformTypeIdentifiers

enum FilterPreset: Int32, CaseIterable {
    case everything = 0
    case audio = 1
    case compressed = 2
    case document = 3
    case executable = 4
    case folder = 5
    case picture = 6
    case video = 7
    case code = 8

    var title: String {
        switch self {
        case .everything: return "Everything"
        case .audio: return "Audio"
        case .compressed: return "Compressed"
        case .document: return "Document"
        case .executable: return "Executable"
        case .folder: return "Folder"
        case .picture: return "Picture"
        case .video: return "Video"
        case .code: return "Code"
        }
    }

    var cValue: SEFilterPreset {
        SEFilterPreset(rawValue: UInt32(self.rawValue))
    }
}

enum SortColumn: Int32 {
    case name = 0
    case path = 1
    case size = 2
    case dateModified = 3
    case dateCreated = 4
    case ext = 5

    var cValue: SESortColumn {
        SESortColumn(rawValue: UInt32(self.rawValue))
    }

    var identifier: NSUserInterfaceItemIdentifier {
        switch self {
        case .name: return NSUserInterfaceItemIdentifier("name")
        case .path: return NSUserInterfaceItemIdentifier("path")
        case .size: return NSUserInterfaceItemIdentifier("size")
        case .dateModified: return NSUserInterfaceItemIdentifier("dateModified")
        case .dateCreated: return NSUserInterfaceItemIdentifier("dateCreated")
        case .ext: return NSUserInterfaceItemIdentifier("ext")
        }
    }

    static func from(identifier: NSUserInterfaceItemIdentifier) -> SortColumn {
        switch identifier.rawValue {
        case "path": return .path
        case "size": return .size
        case "dateModified": return .dateModified
        case "dateCreated": return .dateCreated
        case "ext": return .ext
        default: return .name
        }
    }
}

final class IconCache {
    static let shared = IconCache()
    private var extCache: [String: NSImage] = [:]
    private let folderIcon: NSImage
    private let genericFileIcon: NSImage
    private let appIcon: NSImage
    private let lock = NSLock()

    private init() {
        folderIcon = NSWorkspace.shared.icon(for: .folder)
        folderIcon.size = NSSize(width: 16, height: 16)
        genericFileIcon = NSWorkspace.shared.icon(for: .data)
        genericFileIcon.size = NSSize(width: 16, height: 16)
        appIcon = NSWorkspace.shared.icon(for: .applicationBundle)
        appIcon.size = NSSize(width: 16, height: 16)
    }

    func icon(isDir: Bool, ext: String, name: String) -> NSImage {
        if isDir {
            if name.lowercased().hasSuffix(".app") {
                return appIcon
            }
            return folderIcon
        }
        let key = ext.lowercased()
        if key.isEmpty {
            return genericFileIcon
        }
        lock.lock()
        if let cached = extCache[key] {
            lock.unlock()
            return cached
        }
        lock.unlock()

        let img: NSImage
        if let ut = UTType(filenameExtension: key) {
            img = NSWorkspace.shared.icon(for: ut)
        } else {
            img = genericFileIcon
        }
        img.size = NSSize(width: 16, height: 16)

        lock.lock()
        if extCache.count > 512 {
            extCache.removeAll(keepingCapacity: true)
        }
        extCache[key] = img
        lock.unlock()
        return img
    }
}

final class Formatters {
    static let shared = Formatters()

    let numberFormatter: NumberFormatter = {
        let nf = NumberFormatter()
        nf.numberStyle = .decimal
        return nf
    }()

    let dateFormatter: DateFormatter = {
        let df = DateFormatter()
        df.dateFormat = "yyyy-MM-dd HH:mm"
        return df
    }()

    func formatCount(_ value: UInt64) -> String {
        numberFormatter.string(from: NSNumber(value: value)) ?? "\(value)"
    }

    func formatSize(_ bytes: UInt64, isDir: Bool = false) -> String {
        if isDir && bytes == 0 { return "" }
        if bytes < 1024 {
            return "\(bytes) B"
        }
        let kb = Double(bytes) / 1024.0
        if kb < 1024.0 {
            return String(format: "%.1f KB", kb)
        }
        let mb = kb / 1024.0
        if mb < 1024.0 {
            return String(format: "%.2f MB", mb)
        }
        let gb = mb / 1024.0
        if gb < 1024.0 {
            return String(format: "%.2f GB", gb)
        }
        let tb = gb / 1024.0
        return String(format: "%.2f TB", tb)
    }

    func formatDate(_ epoch: Int64) -> String {
        if epoch <= 0 { return "" }
        return dateFormatter.string(from: Date(timeIntervalSince1970: TimeInterval(epoch)))
    }
}

final class EverythingEngine {
    let raw: OpaquePointer
    private let searchQueue = DispatchQueue(label: "com.seeeverything.search", qos: .userInteractive)
    private var pathBuf = [CChar](repeating: 0, count: 4096)
    private var nameBuf = [CChar](repeating: 0, count: 1024)
    private var extBuf = [CChar](repeating: 0, count: 128)
    private var hlBuf = [SEHighlightRange](repeating: SEHighlightRange(start: 0, length: 0), count: 32)

    var onStatusUpdate: ((SEEngineStats) -> Void)?
    var onIndexChanged: (() -> Void)?

    static var defaultDBPath: String {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSupport.appendingPathComponent("SeeEverything", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("everything.db").path
    }

    init(dbPath: String = EverythingEngine.defaultDBPath) {
        self.raw = dbPath.withCString { se_create($0) }!
        let unmanaged = Unmanaged.passUnretained(self).toOpaque()
        se_set_callbacks(
            raw,
            unmanaged,
            { ctx, stats in
                guard let ctx else { return }
                let me = Unmanaged<EverythingEngine>.fromOpaque(ctx).takeUnretainedValue()
                DispatchQueue.main.async {
                    me.onStatusUpdate?(stats)
                }
            },
            { ctx in
                guard let ctx else { return }
                let me = Unmanaged<EverythingEngine>.fromOpaque(ctx).takeUnretainedValue()
                DispatchQueue.main.async {
                    me.onIndexChanged?()
                }
            }
        )
    }

    deinit {
        se_destroy(raw)
    }

    func start(roots: [String] = ["/"], forceRebuild: Bool = false) {
        let cStrings: [UnsafeMutablePointer<CChar>?] = roots.map { strdup($0) }
        defer { cStrings.forEach { free($0) } }
        var constPtrs: [UnsafePointer<CChar>?] = cStrings.map { UnsafePointer($0) }
        constPtrs.withUnsafeMutableBufferPointer { buf in
            se_start(raw, buf.baseAddress, roots.count, forceRebuild)
        }
    }

    func rebuild() {
        se_rebuild(raw)
    }

    func saveDB() {
        se_save_db(raw)
    }

    func stats() -> SEEngineStats {
        se_get_stats(raw)
    }

    var hasFullDiskAccess: Bool {
        se_has_full_disk_access()
    }

    func searchAsync(query: String, options: SEQueryOptions, completion: @escaping (SEQuerySummary) -> Void) {
        searchQueue.async { [weak self] in
            guard let self else { return }
            var gen: UInt64 = 0
            let summary = query.withCString { qPtr in
                se_search(self.raw, qPtr, options, &gen)
            }
            if summary.generation != 0 {
                DispatchQueue.main.async {
                    completion(summary)
                }
            }
        }
    }

    func resultCount() -> Int {
        Int(se_result_count(raw))
    }

    func entryInfo(row: Int) -> SEEntryInfo? {
        var info = SEEntryInfo()
        guard se_result_get_info(raw, UInt64(row), &info) else { return nil }
        return info
    }

    func name(row: Int) -> String {
        let n = se_result_get_name(raw, UInt64(row), &nameBuf, nameBuf.count)
        return n > 0 ? String(cString: nameBuf) : ""
    }

    func parentPath(row: Int) -> String {
        let n = se_result_get_parent_path(raw, UInt64(row), &pathBuf, pathBuf.count)
        return n > 0 ? String(cString: pathBuf) : ""
    }

    func fullPath(row: Int) -> String {
        let n = se_result_get_full_path(raw, UInt64(row), &pathBuf, pathBuf.count)
        return n > 0 ? String(cString: pathBuf) : ""
    }

    func ext(row: Int) -> String {
        let n = se_result_get_extension(raw, UInt64(row), &extBuf, extBuf.count)
        return n > 0 ? String(cString: extBuf) : ""
    }

    private static let middleTruncateStyle: NSParagraphStyle = {
        let p = NSMutableParagraphStyle()
        p.lineBreakMode = .byTruncatingMiddle
        p.allowsDefaultTighteningForTruncation = true
        return p
    }()

    func attributedText(for row: Int, text: String, inPath: Bool, baseFont: NSFont, boldFont: NSFont, textColor: NSColor) -> NSAttributedString {
        let attr = NSMutableAttributedString(string: text, attributes: [
            .font: baseFont,
            .foregroundColor: textColor,
            .paragraphStyle: Self.middleTruncateStyle
        ])
        let count = se_result_get_highlights(raw, UInt64(row), inPath, &hlBuf, hlBuf.count)
        guard count > 0 else { return attr }

        let utf8View = text.utf8
        let totalUTF8 = utf8View.count

        for i in 0..<count {
            let r = hlBuf[i]
            let startByte = Int(r.start)
            let endByte = startByte + Int(r.length)
            guard startByte >= 0, endByte <= totalUTF8, startByte < endByte else { continue }
            if let sIdx = utf8View.index(utf8View.startIndex, offsetBy: startByte, limitedBy: utf8View.endIndex),
               let eIdx = utf8View.index(utf8View.startIndex, offsetBy: endByte, limitedBy: utf8View.endIndex) {
                let nsRange = NSRange(sIdx..<eIdx, in: text)
                attr.addAttributes([
                    .font: boldFont,
                    .backgroundColor: NSColor.controlAccentColor.withAlphaComponent(0.16)
                ], range: nsRange)
            }
        }
        return attr
    }
}
