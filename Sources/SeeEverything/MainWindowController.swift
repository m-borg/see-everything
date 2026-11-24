import AppKit
import EverythingCore
import Quartz
import QuickLookUI

/// Custom NSTableView that handles Return, Space (Quick Look), Cmd+Return, Delete, and Tab cleanly.
final class ResultsTableView: NSTableView {
    var onOpenSelected: (() -> Void)?
    var onRevealSelected: (() -> Void)?
    var onToggleQuickLook: (() -> Void)?
    var onTrashSelected: (() -> Void)?
    var onFocusSearch: (() -> Void)?
    var onCopyFiles: (() -> Void)?
    var onCopyPaths: (() -> Void)?

    override func keyDown(with event: NSEvent) {
        let mods = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        switch event.keyCode {
        case 36, 76: // Return / Enter
            if mods.contains(.command) {
                onRevealSelected?()
            } else {
                onOpenSelected?()
            }
        case 49: // Space -> Quick Look!
            onToggleQuickLook?()
        case 51: // Delete / Backspace
            if mods.contains(.command) {
                onTrashSelected?()
            } else {
                super.keyDown(with: event)
            }
        case 53: // Escape -> back to search box
            onFocusSearch?()
        case 8 where mods == .command: // Cmd+C -> Copy file
            onCopyFiles?()
        case 8 where mods == [.command, .shift]: // Shift+Cmd+C -> Copy full path
            onCopyPaths?()
        default:
            super.keyDown(with: event)
        }
    }
}

/// Search field subclass (keyboard delegation is handled via NSSearchFieldDelegate.control(_:textView:doCommandBy:)).
final class EverythingSearchField: NSSearchField {}

final class MainWindowController: NSWindowController,
                                  NSTableViewDataSource,
                                  NSTableViewDelegate,
                                  NSSearchFieldDelegate,
                                  QLPreviewPanelDataSource,
                                  QLPreviewPanelDelegate {

    let engine: EverythingEngine

    // UI elements
    private let fdaBanner = NSView()
    private let searchField = EverythingSearchField()
    private let filterPopup = NSPopUpButton()
    private let caseButton = NSButton(title: "Aa", target: nil, action: nil)
    private let wholeWordButton = NSButton(title: "W", target: nil, action: nil)
    private let pathButton = NSButton(title: "/", target: nil, action: nil)
    private let regexButton = NSButton(title: ".*", target: nil, action: nil)
    private let previewToggleButton = NSButton(title: "", target: nil, action: nil)

    private let splitView = NSSplitView()
    private let tableScroll = NSScrollView()
    private let tableView = ResultsTableView()
    private let previewPane = PreviewPaneView()

    private let statusLeftLabel = NSTextField(labelWithString: "Starting…")
    private let statusRightLabel = NSTextField(labelWithString: "")
    private let statusDot = NSView()

    // Search state
    private var matchCase = false
    private var matchWholeWord = false
    private var matchPath = false
    private var matchRegex = false
    private var filterPreset: FilterPreset = .everything
    private var sortColumn: SortColumn = .name
    private var sortAscending = true
    private var foldersFirst = true
    private var showPreviewPane = false

    private var currentRowCount = 0
    private var lastQuerySummary = SEQuerySummary()
    private var lastStats = SEEngineStats()
    private var pendingLiveRefresh: DispatchWorkItem?
    private var isLiveScrolling = false
    private var deferredRefreshAfterScroll = false
    private var searchHistory: [String] = []

    private let rowFont = NSFont.systemFont(ofSize: 12.5, weight: .regular)
    private let rowBoldFont = NSFont.systemFont(ofSize: 12.5, weight: .bold)
    private let monoSmallFont = NSFont.monospacedDigitSystemFont(ofSize: 12, weight: .regular)

    private var fdaBannerHeightConstraint: NSLayoutConstraint!

    init(engine: EverythingEngine) {
        self.engine = engine
        let win = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1080, height: 700),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        win.title = "SeeEverything"
        win.minSize = NSSize(width: 640, height: 400)
        win.setFrameAutosaveName("SeeEverythingMainWindow")
        win.center()
        win.isReleasedWhenClosed = false

        super.init(window: win)
        setupUI()
        wireEngine()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    private func setupUI() {
        guard let content = window?.contentView else { return }

        // 1. Full Disk Access warning banner (hidden automatically if FDA is granted)
        fdaBanner.wantsLayer = true
        fdaBanner.layer?.backgroundColor = NSColor.systemOrange.withAlphaComponent(0.14).cgColor
        fdaBanner.translatesAutoresizingMaskIntoConstraints = false

        let fdaText = NSTextField(labelWithString: "Grant Full Disk Access so SeeEverything can index Desktop, Documents, Downloads, and all folders.")
        fdaText.font = .systemFont(ofSize: 12, weight: .medium)
        fdaText.textColor = .labelColor
        fdaText.lineBreakMode = .byTruncatingTail
        fdaText.translatesAutoresizingMaskIntoConstraints = false

        let fdaOpenBtn = NSButton(title: "Open System Settings…", target: self, action: #selector(openFullDiskAccessSettings))
        fdaOpenBtn.bezelStyle = .rounded
        fdaOpenBtn.controlSize = .small
        fdaOpenBtn.translatesAutoresizingMaskIntoConstraints = false

        let fdaRescanBtn = NSButton(title: "Rescan", target: self, action: #selector(rebuildIndex))
        fdaRescanBtn.bezelStyle = .rounded
        fdaRescanBtn.controlSize = .small
        fdaRescanBtn.translatesAutoresizingMaskIntoConstraints = false

        fdaBanner.addSubview(fdaText)
        fdaBanner.addSubview(fdaOpenBtn)
        fdaBanner.addSubview(fdaRescanBtn)
        NSLayoutConstraint.activate([
            fdaText.leadingAnchor.constraint(equalTo: fdaBanner.leadingAnchor, constant: 12),
            fdaText.centerYAnchor.constraint(equalTo: fdaBanner.centerYAnchor),
            fdaText.trailingAnchor.constraint(lessThanOrEqualTo: fdaOpenBtn.leadingAnchor, constant: -10),
            fdaRescanBtn.trailingAnchor.constraint(equalTo: fdaBanner.trailingAnchor, constant: -10),
            fdaRescanBtn.centerYAnchor.constraint(equalTo: fdaBanner.centerYAnchor),
            fdaOpenBtn.trailingAnchor.constraint(equalTo: fdaRescanBtn.leadingAnchor, constant: -6),
            fdaOpenBtn.centerYAnchor.constraint(equalTo: fdaBanner.centerYAnchor)
        ])

        // 2. Top Search Toolbar
        let topBar = NSView()
        topBar.translatesAutoresizingMaskIntoConstraints = false

        searchField.placeholderString = "Search everything… (e.g. ext:pdf invoice | !draft size:>1mb dm:today)"
        searchField.font = .systemFont(ofSize: 13.5)
        searchField.delegate = self
        searchField.sendsSearchStringImmediately = true
        searchField.sendsWholeSearchString = false
        searchField.translatesAutoresizingMaskIntoConstraints = false

        for preset in FilterPreset.allCases {
            filterPopup.addItem(withTitle: preset.title)
            filterPopup.lastItem?.tag = Int(preset.rawValue)
        }
        filterPopup.target = self
        filterPopup.action = #selector(filterChanged)
        filterPopup.controlSize = .regular
        filterPopup.translatesAutoresizingMaskIntoConstraints = false

        configureTogglePill(caseButton, tooltip: "Match Case (⌥⌘C)", action: #selector(toggleMatchCase))
        configureTogglePill(wholeWordButton, tooltip: "Match Whole Word (⌥⌘W)", action: #selector(toggleMatchWholeWord))
        configureTogglePill(pathButton, tooltip: "Match Path (⌥⌘U)", action: #selector(toggleMatchPath))
        configureTogglePill(regexButton, tooltip: "Enable Regex (⌥⌘R)", action: #selector(toggleMatchRegex))

        previewToggleButton.image = NSImage(systemSymbolName: "sidebar.right", accessibilityDescription: "Toggle Preview Pane")
        previewToggleButton.bezelStyle = .texturedRounded
        previewToggleButton.setButtonType(.pushOnPushOff)
        previewToggleButton.toolTip = "Toggle Preview Pane (⌥⌘P)"
        previewToggleButton.target = self
        previewToggleButton.action = #selector(togglePreviewPane)
        previewToggleButton.translatesAutoresizingMaskIntoConstraints = false

        for v in [searchField, caseButton, wholeWordButton, pathButton, regexButton, filterPopup, previewToggleButton] {
            topBar.addSubview(v)
        }

        NSLayoutConstraint.activate([
            searchField.leadingAnchor.constraint(equalTo: topBar.leadingAnchor, constant: 10),
            searchField.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),

            caseButton.leadingAnchor.constraint(equalTo: searchField.trailingAnchor, constant: 8),
            caseButton.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            caseButton.widthAnchor.constraint(equalToConstant: 32),

            wholeWordButton.leadingAnchor.constraint(equalTo: caseButton.trailingAnchor, constant: 3),
            wholeWordButton.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            wholeWordButton.widthAnchor.constraint(equalToConstant: 28),

            pathButton.leadingAnchor.constraint(equalTo: wholeWordButton.trailingAnchor, constant: 3),
            pathButton.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            pathButton.widthAnchor.constraint(equalToConstant: 26),

            regexButton.leadingAnchor.constraint(equalTo: pathButton.trailingAnchor, constant: 3),
            regexButton.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            regexButton.widthAnchor.constraint(equalToConstant: 30),

            filterPopup.leadingAnchor.constraint(equalTo: regexButton.trailingAnchor, constant: 8),
            filterPopup.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            filterPopup.widthAnchor.constraint(equalToConstant: 122),

            previewToggleButton.leadingAnchor.constraint(equalTo: filterPopup.trailingAnchor, constant: 6),
            previewToggleButton.trailingAnchor.constraint(equalTo: topBar.trailingAnchor, constant: -10),
            previewToggleButton.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            previewToggleButton.widthAnchor.constraint(equalToConstant: 30)
        ])

        // 3. Results Table + Optional Preview Split View
        setupTableView()
        tableScroll.documentView = tableView
        tableScroll.hasVerticalScroller = true
        tableScroll.hasHorizontalScroller = true
        tableScroll.autohidesScrollers = true
        tableScroll.borderType = .noBorder

        NotificationCenter.default.addObserver(
            self,
            selector: #selector(scrollViewWillStartLiveScroll),
            name: NSScrollView.willStartLiveScrollNotification,
            object: tableScroll
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(scrollViewDidEndLiveScroll),
            name: NSScrollView.didEndLiveScrollNotification,
            object: tableScroll
        )

        splitView.isVertical = true
        splitView.dividerStyle = .thin
        splitView.translatesAutoresizingMaskIntoConstraints = false
        splitView.addArrangedSubview(tableScroll)
        splitView.addArrangedSubview(previewPane)
        previewPane.isHidden = true

        // 4. Bottom Status Bar (Classic Everything style)
        let statusBar = NSView()
        statusBar.wantsLayer = true
        statusBar.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        statusBar.translatesAutoresizingMaskIntoConstraints = false

        let statusDivider = NSBox()
        statusDivider.boxType = .separator
        statusDivider.translatesAutoresizingMaskIntoConstraints = false

        statusDot.wantsLayer = true
        statusDot.layer?.cornerRadius = 4
        statusDot.layer?.backgroundColor = NSColor.systemOrange.cgColor
        statusDot.translatesAutoresizingMaskIntoConstraints = false

        statusLeftLabel.font = .monospacedDigitSystemFont(ofSize: 11.5, weight: .regular)
        statusLeftLabel.textColor = .secondaryLabelColor
        statusLeftLabel.lineBreakMode = .byTruncatingTail
        statusLeftLabel.translatesAutoresizingMaskIntoConstraints = false

        statusRightLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .medium)
        statusRightLabel.textColor = .secondaryLabelColor
        statusRightLabel.alignment = .right
        statusRightLabel.translatesAutoresizingMaskIntoConstraints = false

        statusBar.addSubview(statusDivider)
        statusBar.addSubview(statusDot)
        statusBar.addSubview(statusLeftLabel)
        statusBar.addSubview(statusRightLabel)

        NSLayoutConstraint.activate([
            statusDivider.topAnchor.constraint(equalTo: statusBar.topAnchor),
            statusDivider.leadingAnchor.constraint(equalTo: statusBar.leadingAnchor),
            statusDivider.trailingAnchor.constraint(equalTo: statusBar.trailingAnchor),

            statusDot.leadingAnchor.constraint(equalTo: statusBar.leadingAnchor, constant: 10),
            statusDot.centerYAnchor.constraint(equalTo: statusBar.centerYAnchor),
            statusDot.widthAnchor.constraint(equalToConstant: 8),
            statusDot.heightAnchor.constraint(equalToConstant: 8),

            statusLeftLabel.leadingAnchor.constraint(equalTo: statusDot.trailingAnchor, constant: 6),
            statusLeftLabel.centerYAnchor.constraint(equalTo: statusBar.centerYAnchor),
            statusLeftLabel.trailingAnchor.constraint(lessThanOrEqualTo: statusRightLabel.leadingAnchor, constant: -12),

            statusRightLabel.trailingAnchor.constraint(equalTo: statusBar.trailingAnchor, constant: -10),
            statusRightLabel.centerYAnchor.constraint(equalTo: statusBar.centerYAnchor)
        ])

        // Layout everything vertically
        let topDivider = NSBox()
        topDivider.boxType = .separator
        topDivider.translatesAutoresizingMaskIntoConstraints = false

        for v in [fdaBanner, topBar, topDivider, splitView, statusBar] {
            content.addSubview(v)
        }

        let hasFDA = engine.hasFullDiskAccess
        fdaBannerHeightConstraint = fdaBanner.heightAnchor.constraint(equalToConstant: hasFDA ? 0 : 34)
        fdaBanner.isHidden = hasFDA

        NSLayoutConstraint.activate([
            fdaBanner.topAnchor.constraint(equalTo: content.topAnchor),
            fdaBanner.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            fdaBanner.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            fdaBannerHeightConstraint,

            topBar.topAnchor.constraint(equalTo: fdaBanner.bottomAnchor),
            topBar.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            topBar.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            topBar.heightAnchor.constraint(equalToConstant: 42),

            topDivider.topAnchor.constraint(equalTo: topBar.bottomAnchor),
            topDivider.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            topDivider.trailingAnchor.constraint(equalTo: content.trailingAnchor),

            splitView.topAnchor.constraint(equalTo: topDivider.bottomAnchor),
            splitView.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            splitView.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            splitView.bottomAnchor.constraint(equalTo: statusBar.topAnchor),

            statusBar.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            statusBar.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            statusBar.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            statusBar.heightAnchor.constraint(equalToConstant: 26)
        ])
    }

    private func configureTogglePill(_ btn: NSButton, tooltip: String, action: Selector) {
        btn.bezelStyle = .texturedRounded
        btn.setButtonType(.pushOnPushOff)
        btn.font = .monospacedSystemFont(ofSize: 11.5, weight: .semibold)
        btn.toolTip = tooltip
        btn.target = self
        btn.action = action
        btn.translatesAutoresizingMaskIntoConstraints = false
    }

    private func setupTableView() {
        tableView.dataSource = self
        tableView.delegate = self
        tableView.rowHeight = 22
        tableView.intercellSpacing = NSSize(width: 6, height: 0)
        tableView.usesAlternatingRowBackgroundColors = true
        tableView.allowsMultipleSelection = true
        tableView.allowsColumnReordering = true
        tableView.allowsColumnResizing = true
        tableView.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
        tableView.style = .plain
        tableView.doubleAction = #selector(openSelectedItems)
        tableView.target = self
        tableView.setDraggingSourceOperationMask([.copy, .link], forLocal: false)

        // Columns matching Voidtools Everything
        let cols: [(SortColumn, String, CGFloat, Bool)] = [
            (.name, "Name", 300, false),
            (.path, "Path", 390, false),
            (.size, "Size", 90, false),
            (.dateModified, "Date Modified", 130, false),
            (.ext, "Extension", 72, false),
            (.dateCreated, "Date Created", 130, true) // hidden by default, toggleable via header menu
        ]

        for (sc, title, width, hidden) in cols {
            let col = NSTableColumn(identifier: sc.identifier)
            col.title = title
            col.width = width
            col.minWidth = 50
            col.isHidden = hidden
            col.sortDescriptorPrototype = NSSortDescriptor(key: sc.identifier.rawValue, ascending: true)
            if sc == .size {
                col.headerCell.alignment = .right
            }
            tableView.addTableColumn(col)
        }

        tableView.sortDescriptors = [NSSortDescriptor(key: SortColumn.name.identifier.rawValue, ascending: true)]

        // Header context menu to toggle columns
        let headerMenu = NSMenu()
        for col in tableView.tableColumns {
            let item = NSMenuItem(title: col.title, action: #selector(toggleColumnVisibility(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = col
            item.state = col.isHidden ? .off : .on
            headerMenu.addItem(item)
        }
        tableView.headerView?.menu = headerMenu

        // Row context menu
        tableView.menu = buildContextMenu()

        // Keyboard handlers
        tableView.onOpenSelected = { [weak self] in self?.openSelectedItems() }
        tableView.onRevealSelected = { [weak self] in self?.revealSelectedInFinder() }
        tableView.onToggleQuickLook = { [weak self] in self?.toggleQuickLook() }
        tableView.onTrashSelected = { [weak self] in self?.moveSelectedToTrash() }
        tableView.onFocusSearch = { [weak self] in self?.focusSearchField() }
        tableView.onCopyFiles = { [weak self] in self?.copySelectedFiles() }
        tableView.onCopyPaths = { [weak self] in self?.copySelectedPaths() }
    }

    private func buildContextMenu() -> NSMenu {
        let m = NSMenu()
        m.addItem(withTitle: "Open", action: #selector(openSelectedItems), keyEquivalent: "")
        m.addItem(withTitle: "Reveal in Finder", action: #selector(revealSelectedInFinder), keyEquivalent: "")
        m.addItem(withTitle: "Quick Look", action: #selector(toggleQuickLook), keyEquivalent: "")
        m.addItem(withTitle: "Open Parent Folder in Terminal", action: #selector(openParentInTerminal), keyEquivalent: "")
        m.addItem(.separator())
        m.addItem(withTitle: "Copy Full Path", action: #selector(copySelectedPaths), keyEquivalent: "")
        m.addItem(withTitle: "Copy Name", action: #selector(copySelectedNames), keyEquivalent: "")
        m.addItem(withTitle: "Copy File", action: #selector(copySelectedFiles), keyEquivalent: "")
        m.addItem(.separator())
        m.addItem(withTitle: "Move to Trash", action: #selector(moveSelectedToTrash), keyEquivalent: "")
        for item in m.items {
            item.target = self
        }
        return m
    }

    private func wireEngine() {
        engine.onStatusUpdate = { [weak self] stats in
            guard let self else { return }
            self.lastStats = stats
            self.refreshStatusBar()
            self.refreshFDABanner()
        }
        engine.onIndexChanged = { [weak self] in
            guard let self else { return }
            // Debounce live FSEvents re-queries and never interrupt active scrolling
            self.pendingLiveRefresh?.cancel()
            let work = DispatchWorkItem { [weak self] in
                guard let self else { return }
                if self.isLiveScrolling {
                    self.deferredRefreshAfterScroll = true
                } else {
                    self.runSearch(preserveSelection: true, resetScroll: false)
                }
            }
            self.pendingLiveRefresh = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: work)
        }
    }

    @objc private func scrollViewWillStartLiveScroll() {
        isLiveScrolling = true
    }

    @objc private func scrollViewDidEndLiveScroll() {
        isLiveScrolling = false
        if deferredRefreshAfterScroll {
            deferredRefreshAfterScroll = false
            runSearch(preserveSelection: true, resetScroll: false)
        }
    }

    func refreshFDABanner() {
        let hasFDA = engine.hasFullDiskAccess
        fdaBanner.isHidden = hasFDA
        fdaBannerHeightConstraint.constant = hasFDA ? 0 : 34
    }

    // MARK: - Search Execution

    private func currentOptions() -> SEQueryOptions {
        SEQueryOptions(
            match_case: matchCase,
            match_whole_word: matchWholeWord,
            match_path: matchPath,
            match_regex: matchRegex,
            filter_preset: filterPreset.cValue,
            sort_column: sortColumn.cValue,
            sort_ascending: sortAscending,
            folders_first: foldersFirst
        )
    }

    func runSearch(preserveSelection: Bool = false, resetScroll: Bool = true) {
        let q = searchField.stringValue
        let opts = currentOptions()
        let selectedPaths: Set<String> = preserveSelection ? Set(selectedFullPaths()) : []

        engine.searchAsync(query: q, options: opts) { [weak self] summary in
            guard let self else { return }
            // If user started scrolling while a live background refresh was running, defer UI reload
            if !resetScroll && self.isLiveScrolling {
                self.deferredRefreshAfterScroll = true
                return
            }

            let savedScrollOrigin = self.tableScroll.contentView.bounds.origin
            self.lastQuerySummary = summary
            self.currentRowCount = self.engine.resultCount()
            self.tableView.reloadData()

            if preserveSelection && !selectedPaths.isEmpty {
                let scanLimit = min(self.currentRowCount, 500)
                let idxs = NSMutableIndexSet()
                for r in 0..<scanLimit {
                    if selectedPaths.contains(self.engine.fullPath(row: r)) {
                        idxs.add(r)
                    }
                }
                if idxs.count > 0 {
                    self.tableView.selectRowIndexes(idxs as IndexSet, byExtendingSelection: false)
                }
            }

            if resetScroll {
                if self.currentRowCount > 0 {
                    self.tableView.scrollRowToVisible(0)
                }
            } else {
                // Keep the exact scroll position during live index refreshes
                let docHeight = self.tableView.frame.height
                let visibleHeight = self.tableScroll.contentView.bounds.height
                let maxY = max(0, docHeight - visibleHeight)
                let clampedOrigin = NSPoint(x: savedScrollOrigin.x, y: min(max(0, savedScrollOrigin.y), maxY))
                self.tableScroll.contentView.scroll(to: clampedOrigin)
                self.tableScroll.reflectScrolledClipView(self.tableScroll.contentView)
            }

            self.refreshStatusBar()
            self.updatePreviewForSelection()
        }
    }

    private func refreshStatusBar() {
        let st = lastStats
        switch st.state {
        case SE_STATE_LOADING_DB:
            statusDot.layer?.backgroundColor = NSColor.systemBlue.cgColor
            statusLeftLabel.stringValue = "Loading saved index…"
        case SE_STATE_INDEXING:
            statusDot.layer?.backgroundColor = NSColor.systemOrange.cgColor
            statusLeftLabel.stringValue = "Indexing disk… \(Formatters.shared.formatCount(st.total_entries)) objects (\(Formatters.shared.formatCount(st.total_files)) files, \(Formatters.shared.formatCount(st.total_folders)) folders)"
        default:
            statusDot.layer?.backgroundColor = st.fsevents_active ? NSColor.systemGreen.cgColor : NSColor.systemGray.cgColor
            let selected = tableView.selectedRowIndexes
            if selected.count == 1, let row = selected.first, row < currentRowCount, let info = engine.entryInfo(row: row) {
                let fp = engine.fullPath(row: row)
                let sz = info.is_dir != 0 ? "Folder" : Formatters.shared.formatSize(info.size)
                statusLeftLabel.stringValue = "\(Formatters.shared.formatCount(lastQuerySummary.total_matches)) objects  |  \(fp) (\(sz))"
            } else if selected.count > 1 {
                var selSize: UInt64 = 0
                for r in selected where r < currentRowCount {
                    if let info = engine.entryInfo(row: r), info.is_dir == 0 {
                        selSize += info.size
                    }
                }
                statusLeftLabel.stringValue = "\(Formatters.shared.formatCount(lastQuerySummary.total_matches)) objects  |  \(selected.count) selected (\(Formatters.shared.formatSize(selSize)))"
            } else {
                let qms = Double(lastQuerySummary.query_time_us) / 1000.0
                let sizeStr = Formatters.shared.formatSize(lastQuerySummary.total_size)
                statusLeftLabel.stringValue = String(
                    format: "%@ objects (%@)  ·  %.2f ms",
                    Formatters.shared.formatCount(lastQuerySummary.total_matches),
                    sizeStr,
                    qms
                )
            }
        }

        // Right-side status indicators (just like Voidtools Everything!)
        var pills: [String] = []
        if matchCase { pills.append("CASE") }
        if matchWholeWord { pills.append("WW") }
        if matchPath { pills.append("PATH") }
        if matchRegex { pills.append("REGEX") }
        if filterPreset != .everything { pills.append(filterPreset.title.uppercased()) }
        let memMB = Double(st.memory_bytes) / (1024.0 * 1024.0)
        if st.state == SE_STATE_READY {
            pills.append(String(format: "%.0f MB", memMB))
            if st.fsevents_active { pills.append("LIVE") }
        }
        statusRightLabel.stringValue = pills.joined(separator: "  ·  ")
    }

    // MARK: - NSTableViewDataSource

    func numberOfRows(in tableView: NSTableView) -> Int {
        currentRowCount
    }

    func tableView(_ tableView: NSTableView, pasteboardWriterForRow row: Int) -> NSPasteboardWriting? {
        guard row >= 0, row < currentRowCount else { return nil }
        let p = engine.fullPath(row: row)
        guard !p.isEmpty else { return nil }
        return URL(fileURLWithPath: p) as NSURL
    }

    func tableView(_ tableView: NSTableView, sortDescriptorsDidChange oldDescriptors: [NSSortDescriptor]) {
        guard let desc = tableView.sortDescriptors.first, let key = desc.key else { return }
        sortColumn = SortColumn.from(identifier: NSUserInterfaceItemIdentifier(key))
        sortAscending = desc.ascending
        runSearch(preserveSelection: true)
    }

    // MARK: - NSTableViewDelegate

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard let col = tableColumn, row < currentRowCount, let info = engine.entryInfo(row: row) else {
            return nil
        }

        let colID = col.identifier
        let isDir = info.is_dir != 0
        let isHidden = info.is_hidden != 0
        let textColor: NSColor = isHidden ? .secondaryLabelColor : .labelColor

        if colID == SortColumn.name.identifier {
            let cellID = NSUserInterfaceItemIdentifier("NameCell")
            let cell: NSTableCellView
            if let reused = tableView.makeView(withIdentifier: cellID, owner: self) as? NSTableCellView {
                cell = reused
            } else {
                cell = NSTableCellView()
                cell.identifier = cellID
                let iv = NSImageView()
                iv.translatesAutoresizingMaskIntoConstraints = false
                let tf = NSTextField(labelWithString: "")
                tf.usesSingleLineMode = true
                tf.maximumNumberOfLines = 1
                tf.lineBreakMode = .byTruncatingMiddle
                tf.cell?.wraps = false
                tf.cell?.isScrollable = false
                tf.cell?.truncatesLastVisibleLine = true
                tf.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
                tf.translatesAutoresizingMaskIntoConstraints = false
                cell.addSubview(iv)
                cell.addSubview(tf)
                cell.imageView = iv
                cell.textField = tf
                NSLayoutConstraint.activate([
                    iv.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 4),
                    iv.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
                    iv.widthAnchor.constraint(equalToConstant: 16),
                    iv.heightAnchor.constraint(equalToConstant: 16),
                    tf.leadingAnchor.constraint(equalTo: iv.trailingAnchor, constant: 6),
                    tf.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -4),
                    tf.centerYAnchor.constraint(equalTo: cell.centerYAnchor)
                ])
            }
            let name = engine.name(row: row)
            let ext = isDir ? "" : engine.ext(row: row)
            cell.imageView?.image = IconCache.shared.icon(isDir: isDir, ext: ext, name: name)
            cell.imageView?.alphaValue = isHidden ? 0.55 : 1.0
            cell.textField?.attributedStringValue = engine.attributedText(
                for: row, text: name, inPath: false,
                baseFont: rowFont, boldFont: rowBoldFont, textColor: textColor
            )
            return cell
        }

        let cellID = NSUserInterfaceItemIdentifier("TextCell_\(colID.rawValue)")
        let cell: NSTableCellView
        if let reused = tableView.makeView(withIdentifier: cellID, owner: self) as? NSTableCellView {
            cell = reused
        } else {
            cell = NSTableCellView()
            cell.identifier = cellID
            let tf = NSTextField(labelWithString: "")
            tf.usesSingleLineMode = true
            tf.maximumNumberOfLines = 1
            tf.lineBreakMode = (colID == SortColumn.path.identifier) ? .byTruncatingMiddle : .byTruncatingTail
            tf.cell?.wraps = false
            tf.cell?.isScrollable = false
            tf.cell?.truncatesLastVisibleLine = true
            tf.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            tf.translatesAutoresizingMaskIntoConstraints = false
            cell.addSubview(tf)
            cell.textField = tf
            NSLayoutConstraint.activate([
                tf.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 4),
                tf.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -4),
                tf.centerYAnchor.constraint(equalTo: cell.centerYAnchor)
            ])
        }

        switch colID {
        case SortColumn.path.identifier:
            let p = engine.parentPath(row: row)
            if matchPath {
                cell.textField?.attributedStringValue = engine.attributedText(
                    for: row, text: p, inPath: true,
                    baseFont: rowFont, boldFont: rowBoldFont, textColor: .secondaryLabelColor
                )
            } else {
                cell.textField?.font = rowFont
                cell.textField?.textColor = .secondaryLabelColor
                cell.textField?.stringValue = p
            }
        case SortColumn.size.identifier:
            cell.textField?.font = monoSmallFont
            cell.textField?.alignment = .right
            cell.textField?.textColor = .secondaryLabelColor
            cell.textField?.stringValue = Formatters.shared.formatSize(info.size, isDir: isDir)
        case SortColumn.dateModified.identifier:
            cell.textField?.font = monoSmallFont
            cell.textField?.textColor = .secondaryLabelColor
            cell.textField?.stringValue = Formatters.shared.formatDate(info.mtime)
        case SortColumn.dateCreated.identifier:
            cell.textField?.font = monoSmallFont
            cell.textField?.textColor = .secondaryLabelColor
            cell.textField?.stringValue = Formatters.shared.formatDate(info.ctime)
        case SortColumn.ext.identifier:
            cell.textField?.font = rowFont
            cell.textField?.textColor = .secondaryLabelColor
            cell.textField?.stringValue = isDir ? "" : engine.ext(row: row)
        default:
            break
        }
        return cell
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        refreshStatusBar()
        updatePreviewForSelection()
        if QLPreviewPanel.sharedPreviewPanelExists(), let panel = QLPreviewPanel.shared(), panel.isVisible {
            panel.reloadData()
        }
    }

    private func updatePreviewForSelection() {
        guard showPreviewPane else { return }
        let row = tableView.selectedRow
        if row >= 0, row < currentRowCount, let info = engine.entryInfo(row: row) {
            previewPane.update(
                path: engine.fullPath(row: row),
                isDir: info.is_dir != 0,
                size: info.size,
                mtime: info.mtime,
                isDataless: info.is_dataless != 0
            )
        } else {
            previewPane.update(path: nil, isDir: false, size: 0, mtime: 0, isDataless: false)
        }
    }

    // MARK: - NSSearchFieldDelegate

    func controlTextDidChange(_ obj: Notification) {
        runSearch()
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
        if commandSelector == #selector(NSResponder.moveDown(_:)) {
            focusResultsFirstRow()
            return true
        }
        if commandSelector == #selector(NSResponder.insertNewline(_:)) {
            openFirstOrSelected()
            return true
        }
        return false
    }

    // MARK: - Selection Helpers

    private func targetRows() -> [Int] {
        let clicked = tableView.clickedRow
        if clicked >= 0 && !tableView.selectedRowIndexes.contains(clicked) {
            return [clicked]
        }
        return tableView.selectedRowIndexes.filter { $0 >= 0 && $0 < currentRowCount }
    }

    private func selectedFullPaths() -> [String] {
        targetRows().map { engine.fullPath(row: $0) }.filter { !$0.isEmpty }
    }

    // MARK: - Actions

    @objc func focusSearchField() {
        window?.makeFirstResponder(searchField)
        searchField.currentEditor()?.selectAll(nil)
    }

    private func focusResultsFirstRow() {
        guard currentRowCount > 0 else { return }
        if tableView.selectedRowIndexes.isEmpty {
            tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
            tableView.scrollRowToVisible(0)
        }
        window?.makeFirstResponder(tableView)
    }

    private func openFirstOrSelected() {
        let q = searchField.stringValue.trimmingCharacters(in: .whitespaces)
        if !q.isEmpty && searchHistory.first != q {
            searchHistory.insert(q, at: 0)
            if searchHistory.count > 30 { searchHistory.removeLast() }
        }
        if tableView.selectedRowIndexes.isEmpty && currentRowCount > 0 {
            let p = engine.fullPath(row: 0)
            if !p.isEmpty {
                NSWorkspace.shared.open(URL(fileURLWithPath: p))
            }
        } else {
            openSelectedItems()
        }
    }

    @objc func openSelectedItems() {
        for p in selectedFullPaths() {
            NSWorkspace.shared.open(URL(fileURLWithPath: p))
        }
    }

    @objc func revealSelectedInFinder() {
        let urls = selectedFullPaths().map { URL(fileURLWithPath: $0) }
        guard !urls.isEmpty else { return }
        NSWorkspace.shared.activateFileViewerSelecting(urls)
    }

    @objc func openParentInTerminal() {
        guard let first = selectedFullPaths().first else { return }
        let url = URL(fileURLWithPath: first)
        var isDir: ObjCBool = false
        FileManager.default.fileExists(atPath: first, isDirectory: &isDir)
        let dir = isDir.boolValue ? url : url.deletingLastPathComponent()
        if let termURL = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.Terminal") {
            let cfg = NSWorkspace.OpenConfiguration()
            NSWorkspace.shared.open([dir], withApplicationAt: termURL, configuration: cfg)
        }
    }

    @objc func copySelectedPaths() {
        let paths = selectedFullPaths()
        guard !paths.isEmpty else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(paths.joined(separator: "\n"), forType: .string)
    }

    @objc func copySelectedNames() {
        let names = targetRows().map { engine.name(row: $0) }.filter { !$0.isEmpty }
        guard !names.isEmpty else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(names.joined(separator: "\n"), forType: .string)
    }

    @objc func copySelectedFiles() {
        let urls = selectedFullPaths().map { URL(fileURLWithPath: $0) as NSURL }
        guard !urls.isEmpty else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.writeObjects(urls)
    }

    @objc func moveSelectedToTrash() {
        let urls = selectedFullPaths().map { URL(fileURLWithPath: $0) }
        guard !urls.isEmpty else { return }
        NSWorkspace.shared.recycle(urls) { _, _ in }
    }

    @objc func toggleMatchCase() {
        matchCase.toggle()
        caseButton.state = matchCase ? .on : .off
        runSearch()
    }

    @objc func toggleMatchWholeWord() {
        matchWholeWord.toggle()
        wholeWordButton.state = matchWholeWord ? .on : .off
        runSearch()
    }

    @objc func toggleMatchPath() {
        matchPath.toggle()
        pathButton.state = matchPath ? .on : .off
        runSearch()
    }

    @objc func toggleMatchRegex() {
        matchRegex.toggle()
        regexButton.state = matchRegex ? .on : .off
        runSearch()
    }

    @objc func toggleFoldersFirst() {
        foldersFirst.toggle()
        runSearch(preserveSelection: true)
    }

    @objc func filterChanged() {
        let raw = Int32(filterPopup.selectedTag())
        filterPreset = FilterPreset(rawValue: raw) ?? .everything
        runSearch()
    }

    func selectFilter(_ preset: FilterPreset) {
        filterPreset = preset
        filterPopup.selectItem(withTag: Int(preset.rawValue))
        runSearch()
    }

    @objc func togglePreviewPane() {
        showPreviewPane.toggle()
        previewToggleButton.state = showPreviewPane ? .on : .off
        previewPane.isHidden = !showPreviewPane
        if showPreviewPane {
            let totalW = splitView.bounds.width
            splitView.setPosition(max(360, totalW - 320), ofDividerAt: 0)
            updatePreviewForSelection()
        }
    }

    @objc func toggleColumnVisibility(_ sender: NSMenuItem) {
        guard let col = sender.representedObject as? NSTableColumn else { return }
        col.isHidden.toggle()
        sender.state = col.isHidden ? .off : .on
    }

    @objc func rebuildIndex() {
        refreshFDABanner()
        engine.rebuild()
    }

    @objc func openFullDiskAccessSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles") {
            NSWorkspace.shared.open(url)
        }
    }

    // MARK: - Quick Look (Spacebar)

    @objc func toggleQuickLook() {
        guard let panel = QLPreviewPanel.shared() else { return }
        if QLPreviewPanel.sharedPreviewPanelExists() && panel.isVisible {
            panel.orderOut(nil)
        } else if !selectedFullPaths().isEmpty {
            panel.makeKeyAndOrderFront(nil)
            panel.reloadData()
        }
    }

    override func acceptsPreviewPanelControl(_ panel: QLPreviewPanel!) -> Bool {
        true
    }

    override func beginPreviewPanelControl(_ panel: QLPreviewPanel!) {
        panel.dataSource = self
        panel.delegate = self
    }

    override func endPreviewPanelControl(_ panel: QLPreviewPanel!) {
        panel.dataSource = nil
        panel.delegate = nil
    }

    func numberOfPreviewItems(in panel: QLPreviewPanel!) -> Int {
        selectedFullPaths().count
    }

    func previewPanel(_ panel: QLPreviewPanel!, previewItemAt index: Int) -> QLPreviewItem! {
        let paths = selectedFullPaths()
        guard index >= 0 && index < paths.count else { return nil }
        return URL(fileURLWithPath: paths[index]) as NSURL
    }
}
