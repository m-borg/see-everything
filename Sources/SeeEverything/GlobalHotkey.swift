import Carbon
import Foundation

/// Registers a system-wide macOS hotkey (Option+Space by default) via Carbon's
/// RegisterEventHotKey, which requires NO Accessibility permissions.
final class GlobalHotkeyManager {
    private var hotKeyRef: EventHotKeyRef?
    private var eventHandler: EventHandlerRef?
    private let onTrigger: () -> Void

    init(keyCode: UInt32 = UInt32(kVK_Space), modifiers: UInt32 = UInt32(optionKey), onTrigger: @escaping () -> Void) {
        self.onTrigger = onTrigger
        register(keyCode: keyCode, modifiers: modifiers)
    }

    deinit {
        unregister()
    }

    private func register(keyCode: UInt32, modifiers: UInt32) {
        var eventSpec = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed)
        )
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()

        InstallEventHandler(
            GetApplicationEventTarget(),
            { _, _, userData -> OSStatus in
                guard let userData else { return noErr }
                let mgr = Unmanaged<GlobalHotkeyManager>.fromOpaque(userData).takeUnretainedValue()
                DispatchQueue.main.async {
                    mgr.onTrigger()
                }
                return noErr
            },
            1,
            &eventSpec,
            selfPtr,
            &eventHandler
        )

        let hotKeyID = EventHotKeyID(signature: OSType(0x53454556), id: 1) // 'SEEV'
        RegisterEventHotKey(keyCode, modifiers, hotKeyID, GetApplicationEventTarget(), 0, &hotKeyRef)
    }

    func unregister() {
        if let ref = hotKeyRef {
            UnregisterEventHotKey(ref)
            hotKeyRef = nil
        }
        if let handler = eventHandler {
            RemoveEventHandler(handler)
            eventHandler = nil
        }
    }
}
