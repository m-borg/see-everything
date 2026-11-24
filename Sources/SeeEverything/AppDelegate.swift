import AppKit
import EverythingCore

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var engine: EverythingEngine!
    private var windowController: MainWindowController!
    private var statusItem: NSStatusItem!
    private var hotkeyManager: GlobalHotkeyManager!

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)

        engine = EverythingEngine()
        windowController = MainWindowController(engine: engine)
        setupMainMenu()
        setupStatusBarIcon()

        // Global Option+Space hotkey to toggle SeeEverything instantly from anywhere
        hotkeyManager = GlobalHotkeyManager { [weak self] in
            self?.toggleMainWindow()
        }

        windowController.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
        windowController.focusSearchField()

        // Index root filesystem + any mounted local volumes under /Volumes
        engine.start(roots: ["/"], forceRebuild: false)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showMainWindow()
        return true
    }

    func applicationWillTerminate(_ notification: Notification) {
        hotkeyManager?.unregister()
        engine.saveDB()
    }

    private func setupStatusBarIcon() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "magnifyingglass", accessibilityDescription: "SeeEverything")
            button.toolTip = "SeeEverything (⌥Space)"
        }
        let menu = NSMenu()
        menu.addItem(withTitle: "Show SeeEverything (⌥Space)", action: #selector(showMainWindow), keyEquivalent: "")
        menu.addItem(.separator())
        menu.addItem(withTitle: "Force Rebuild Index", action: #selector(rebuildIndex), keyEquivalent: "")
        menu.addItem(withTitle: "Full Disk Access Settings…", action: #selector(openFDASettings), keyEquivalent: "")
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit SeeEverything", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        for item in menu.items where item.target == nil && item.action != #selector(NSApplication.terminate(_:)) {
            item.target = self
        }
        statusItem.menu = menu
    }

    private func setupMainMenu() {
        let mainMenu = NSMenu()

        // App menu
        let appItem = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About SeeEverything", action: #selector(showAbout), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide SeeEverything", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit SeeEverything", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        mainMenu.addItem(appItem)

        // File menu
        let fileItem = NSMenuItem()
        let fileMenu = NSMenu(title: "File")
        fileMenu.addItem(withTitle: "Open", action: #selector(MainWindowController.openSelectedItems), keyEquivalent: "o")
        fileMenu.addItem(withTitle: "Reveal in Finder", action: #selector(MainWindowController.revealSelectedInFinder), keyEquivalent: "\r")
        fileMenu.addItem(withTitle: "Quick Look", action: #selector(MainWindowController.toggleQuickLook), keyEquivalent: "y")
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "Move to Trash", action: #selector(MainWindowController.moveSelectedToTrash), keyEquivalent: "\u{08}")
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        fileItem.submenu = fileMenu
        mainMenu.addItem(fileItem)

        // Edit menu
        let editItem = NSMenuItem()
        let editMenu = NSMenu(title: "Edit")
        editMenu.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        editMenu.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        editMenu.addItem(.separator())
        editMenu.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        editMenu.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        let copyPathItem = NSMenuItem(title: "Copy Full Path", action: #selector(MainWindowController.copySelectedPaths), keyEquivalent: "C")
        copyPathItem.keyEquivalentModifierMask = [.command, .shift]
        editMenu.addItem(copyPathItem)
        editMenu.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = editMenu
        mainMenu.addItem(editItem)

        // Search menu (Classic Everything Search menu!)
        let searchItem = NSMenuItem()
        let searchMenu = NSMenu(title: "Search")
        let focusItem = NSMenuItem(title: "Focus Search Bar", action: #selector(MainWindowController.focusSearchField), keyEquivalent: "f")
        searchMenu.addItem(focusItem)
        let focusItem2 = NSMenuItem(title: "Jump to Search Bar", action: #selector(MainWindowController.focusSearchField), keyEquivalent: "l")
        searchMenu.addItem(focusItem2)
        searchMenu.addItem(.separator())

        let caseItem = NSMenuItem(title: "Match Case", action: #selector(MainWindowController.toggleMatchCase), keyEquivalent: "c")
        caseItem.keyEquivalentModifierMask = [.command, .option]
        searchMenu.addItem(caseItem)

        let wwItem = NSMenuItem(title: "Match Whole Word", action: #selector(MainWindowController.toggleMatchWholeWord), keyEquivalent: "w")
        wwItem.keyEquivalentModifierMask = [.command, .option]
        searchMenu.addItem(wwItem)

        let pathItem = NSMenuItem(title: "Match Path", action: #selector(MainWindowController.toggleMatchPath), keyEquivalent: "u")
        pathItem.keyEquivalentModifierMask = [.command, .option]
        searchMenu.addItem(pathItem)

        let regexItem = NSMenuItem(title: "Enable Regex", action: #selector(MainWindowController.toggleMatchRegex), keyEquivalent: "r")
        regexItem.keyEquivalentModifierMask = [.command, .option]
        searchMenu.addItem(regexItem)

        searchMenu.addItem(.separator())
        for (idx, preset) in FilterPreset.allCases.enumerated() {
            let key = idx < 9 ? "\(idx + 1)" : ""
            let mi = NSMenuItem(title: preset.title, action: #selector(selectFilterMenuItem(_:)), keyEquivalent: key)
            mi.tag = Int(preset.rawValue)
            mi.target = self
            searchMenu.addItem(mi)
        }
        searchItem.submenu = searchMenu
        mainMenu.addItem(searchItem)

        // View menu
        let viewItem = NSMenuItem()
        let viewMenu = NSMenu(title: "View")
        let prevItem = NSMenuItem(title: "Toggle Preview Pane", action: #selector(MainWindowController.togglePreviewPane), keyEquivalent: "p")
        prevItem.keyEquivalentModifierMask = [.command, .option]
        viewMenu.addItem(prevItem)
        viewMenu.addItem(withTitle: "Folders First", action: #selector(MainWindowController.toggleFoldersFirst), keyEquivalent: "")
        viewMenu.addItem(.separator())
        viewMenu.addItem(withTitle: "Force Rebuild Index", action: #selector(MainWindowController.rebuildIndex), keyEquivalent: "r")
        viewItem.submenu = viewMenu
        mainMenu.addItem(viewItem)

        // Help menu (Quick syntax cheat sheet!)
        let helpItem = NSMenuItem()
        let helpMenu = NSMenu(title: "Help")
        let syntaxItem = NSMenuItem(title: "Search Syntax Guide", action: #selector(showSyntaxHelp), keyEquivalent: "?")
        syntaxItem.target = self
        helpMenu.addItem(syntaxItem)
        helpItem.submenu = helpMenu
        mainMenu.addItem(helpItem)

        NSApp.mainMenu = mainMenu
    }

    @objc private func showMainWindow() {
        windowController.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
        windowController.focusSearchField()
    }

    @objc private func toggleMainWindow() {
        if let win = windowController.window, win.isVisible && NSApp.isActive {
            win.orderOut(nil)
        } else {
            showMainWindow()
        }
    }

    @objc private func rebuildIndex() {
        windowController.rebuildIndex()
    }

    @objc private func openFDASettings() {
        windowController.openFullDiskAccessSettings()
    }

    @objc private func selectFilterMenuItem(_ sender: NSMenuItem) {
        if let preset = FilterPreset(rawValue: Int32(sender.tag)) {
            windowController.selectFilter(preset)
        }
    }

    @objc private func showAbout() {
        let alert = NSAlert()
        alert.messageText = "SeeEverything"
        alert.informativeText = """
        Native macOS instant file search inspired by Voidtools Everything.

        • Parallel getattrlistbulk() APFS indexer
        • Persistent binary index + FSEvents real-time sync
        • Full Everything query syntax & instant sorting
        • Global Hotkey: ⌥Space
        """
        alert.runModal()
    }

    @objc private func showSyntaxHelp() {
        let alert = NSAlert()
        alert.messageText = "Everything Search Syntax"
        alert.informativeText = """
        Operators:
          foo bar          AND (both terms)
          foo | bar        OR (either term)
          !foo             NOT (exclude term)
          "my file"        Exact phrase with spaces
          <a | b> c        Grouping
          *.pdf, img_??    Wildcards (* and ?)
          Downloads/pdf    Contains '/' → matches full path

        Functions & Modifiers:
          ext:pdf;docx     Filter by extension(s)
          file: / folder:  Files only / Folders only
          size:>10mb       Size (empty, tiny, small, medium, large, huge, gigantic, 1mb..50mb)
          dm:today         Date modified (today, yesterday, thisweek, thismonth, last24hours, >2026-01-01)
          dc:thisyear      Date created
          parent:/Users/.. Exact parent folder
          startwith: / endwith: / len:>40 / depth:3 / empty:
          case: / ww: / path: / regex:

        Shortcuts:
          ⌥Space           Toggle SeeEverything anywhere
          Space            Quick Look preview
          Return / ⌘Return Open / Reveal in Finder
          ⇧⌘C              Copy full path
          ⌥⌘P              Toggle Preview Pane
        """
        alert.runModal()
    }
}
