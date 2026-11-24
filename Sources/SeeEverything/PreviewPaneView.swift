import AppKit
import PDFKit
import QuickLookThumbnailing

/// Native side Preview Pane (toggled with Option+Cmd+P, matching Everything's Alt+P).
final class PreviewPaneView: NSView {
    private let headerLabel = NSTextField(labelWithString: "Preview")
    private let metaLabel = NSTextField(wrappingLabelWithString: "")
    private let imageView = NSImageView()
    private let textScroll = NSScrollView()
    private let textView = NSTextView()
    private let pdfView = PDFView()
    private let emptyLabel = NSTextField(labelWithString: "Select a file to preview")
    private var currentPath: String = ""

    private static let imageExts: Set<String> = [
        "png", "jpg", "jpeg", "gif", "webp", "heic", "heif", "bmp", "tiff", "tif", "ico", "icns", "svg"
    ]
    private static let textExts: Set<String> = [
        "txt", "md", "markdown", "json", "yaml", "yml", "toml", "ini", "cfg", "conf", "log",
        "swift", "c", "h", "hpp", "cpp", "cc", "m", "mm", "rs", "go", "py", "js", "ts", "jsx", "tsx",
        "html", "css", "scss", "xml", "sql", "sh", "zsh", "bash", "rb", "java", "kt", "csv", "plist",
        "gitignore", "env", "dockerfile", "makefile", "lua", "php", "r"
    ]

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setupUI()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    private func setupUI() {
        wantsLayer = true
        layer?.backgroundColor = NSColor.controlBackgroundColor.cgColor

        headerLabel.font = .systemFont(ofSize: 12, weight: .semibold)
        headerLabel.textColor = .labelColor
        headerLabel.lineBreakMode = .byTruncatingMiddle

        metaLabel.font = .systemFont(ofSize: 11)
        metaLabel.textColor = .secondaryLabelColor
        metaLabel.maximumNumberOfLines = 4

        imageView.imageScaling = .scaleProportionallyDown
        imageView.imageAlignment = .alignCenter
        imageView.isHidden = true

        textView.isEditable = false
        textView.isSelectable = true
        textView.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        textView.backgroundColor = .textBackgroundColor
        textView.textColor = .textColor
        textView.autoresizingMask = [.width]
        textScroll.documentView = textView
        textScroll.hasVerticalScroller = true
        textScroll.hasHorizontalScroller = true
        textScroll.borderType = .noBorder
        textScroll.isHidden = true

        pdfView.autoScales = true
        pdfView.displayMode = .singlePageContinuous
        pdfView.isHidden = true

        emptyLabel.font = .systemFont(ofSize: 12)
        emptyLabel.textColor = .tertiaryLabelColor
        emptyLabel.alignment = .center

        let divider = NSBox()
        divider.boxType = .separator

        for v in [headerLabel, metaLabel, divider, imageView, textScroll, pdfView, emptyLabel] {
            v.translatesAutoresizingMaskIntoConstraints = false
            addSubview(v)
        }

        NSLayoutConstraint.activate([
            headerLabel.topAnchor.constraint(equalTo: topAnchor, constant: 10),
            headerLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            headerLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),

            metaLabel.topAnchor.constraint(equalTo: headerLabel.bottomAnchor, constant: 4),
            metaLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            metaLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),

            divider.topAnchor.constraint(equalTo: metaLabel.bottomAnchor, constant: 8),
            divider.leadingAnchor.constraint(equalTo: leadingAnchor),
            divider.trailingAnchor.constraint(equalTo: trailingAnchor),

            imageView.topAnchor.constraint(equalTo: divider.bottomAnchor, constant: 12),
            imageView.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            imageView.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            imageView.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12),

            textScroll.topAnchor.constraint(equalTo: divider.bottomAnchor),
            textScroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            textScroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            textScroll.bottomAnchor.constraint(equalTo: bottomAnchor),

            pdfView.topAnchor.constraint(equalTo: divider.bottomAnchor),
            pdfView.leadingAnchor.constraint(equalTo: leadingAnchor),
            pdfView.trailingAnchor.constraint(equalTo: trailingAnchor),
            pdfView.bottomAnchor.constraint(equalTo: bottomAnchor),

            emptyLabel.centerXAnchor.constraint(equalTo: centerXAnchor),
            emptyLabel.centerYAnchor.constraint(equalTo: centerYAnchor, constant: 20)
        ])
    }

    func update(path: String?, isDir: Bool, size: UInt64, mtime: Int64, isDataless: Bool) {
        guard let path, !path.isEmpty else {
            currentPath = ""
            headerLabel.stringValue = "Preview"
            metaLabel.stringValue = ""
            hideAllPreviews()
            emptyLabel.stringValue = "Select a file to preview"
            emptyLabel.isHidden = false
            return
        }
        if path == currentPath { return }
        currentPath = path

        let url = URL(fileURLWithPath: path)
        headerLabel.stringValue = url.lastPathComponent
        let sizeStr = isDir ? "Folder" : Formatters.shared.formatSize(size)
        let modStr = Formatters.shared.formatDate(mtime)
        let cloudTag = isDataless ? " · iCloud (not downloaded)" : ""
        metaLabel.stringValue = "\(sizeStr) · Modified \(modStr)\(cloudTag)\n\(url.deletingLastPathComponent().path)"

        hideAllPreviews()

        if isDataless {
            emptyLabel.stringValue = "iCloud placeholder (not downloaded)\nPress Space for Quick Look"
            emptyLabel.isHidden = false
            return
        }

        if isDir {
            imageView.image = NSWorkspace.shared.icon(forFile: path)
            imageView.image?.size = NSSize(width: 128, height: 128)
            imageView.isHidden = false
            return
        }

        let ext = url.pathExtension.lowercased()

        if ext == "pdf" {
            if let doc = PDFDocument(url: url) {
                pdfView.document = doc
                pdfView.isHidden = false
                return
            }
        }

        if Self.imageExts.contains(ext) {
            if let img = NSImage(contentsOf: url) {
                imageView.image = img
                imageView.isHidden = false
                return
            }
        }

        if Self.textExts.contains(ext) || (ext.isEmpty && size > 0 && size < 256_000) {
            if let handle = try? FileHandle(forReadingFrom: url) {
                let data = handle.readData(ofLength: 64 * 1024)
                try? handle.close()
                if let str = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) {
                    let truncated = data.count >= 64 * 1024 ? "\n\n… (truncated at 64 KB)" : ""
                    textView.string = str + truncated
                    textScroll.isHidden = false
                    return
                }
            }
        }

        // Fallback: QuickLook thumbnail generation (handles videos, Pages, Keynote, Office, fonts, etc.)
        let request = QLThumbnailGenerator.Request(
            fileAt: url,
            size: CGSize(width: 320, height: 320),
            scale: window?.backingScaleFactor ?? 2.0,
            representationTypes: .all
        )
        let expectedPath = path
        QLThumbnailGenerator.shared.generateBestRepresentation(for: request) { [weak self] rep, _ in
            DispatchQueue.main.async {
                guard let self, self.currentPath == expectedPath else { return }
                if let cg = rep?.cgImage {
                    self.imageView.image = NSImage(cgImage: cg, size: NSSize(width: cg.width, height: cg.height))
                    self.imageView.isHidden = false
                    self.emptyLabel.isHidden = true
                } else {
                    self.imageView.image = NSWorkspace.shared.icon(forFile: expectedPath)
                    self.imageView.isHidden = false
                    self.emptyLabel.isHidden = true
                }
            }
        }
    }

    private func hideAllPreviews() {
        imageView.isHidden = true
        textScroll.isHidden = true
        pdfView.isHidden = true
        emptyLabel.isHidden = true
    }
}
