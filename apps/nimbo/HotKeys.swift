import Carbon.HIToolbox

// Global hotkeys through Carbon: no Accessibility permission required.
final class HotKeys {
    private var refs: [EventHotKeyRef?] = []
    private var handlers: [UInt32: () -> Void] = [:]
    private var eventHandler: EventHandlerRef?
    private static var nextID: UInt32 = 1  // unique across instances (server and app both register)

    init() {
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, event, user in
            guard let event, let user else { return OSStatus(eventNotHandledErr) }
            var hk = EventHotKeyID()
            GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID),
                              nil, MemoryLayout<EventHotKeyID>.size, nil, &hk)
            let me = Unmanaged<HotKeys>.fromOpaque(user).takeUnretainedValue()
            // Not ours (another instance's hotkey): let the next handler in the chain run it.
            guard let action = me.handlers[hk.id] else { return OSStatus(eventNotHandledErr) }
            action()
            return noErr
        }, 1, &spec, Unmanaged.passUnretained(self).toOpaque(), &eventHandler)
    }

    deinit {
        refs.forEach { if let r = $0 { UnregisterEventHotKey(r) } }
        if let eventHandler { RemoveEventHandler(eventHandler) }
    }

    // keyCode: kVK_ANSI_*; modifiers: cmdKey | optionKey ...
    @discardableResult
    func register(keyCode: Int, modifiers: Int, _ action: @escaping () -> Void) -> Bool {
        let id = HotKeys.nextID
        HotKeys.nextID += 1
        var ref: EventHotKeyRef?
        let status = RegisterEventHotKey(UInt32(keyCode), UInt32(modifiers),
                                         EventHotKeyID(signature: OSType(0x524E_4743), id: id),  // 'RNGC'
                                         GetApplicationEventTarget(), 0, &ref)
        guard status == noErr else { return false }
        refs.append(ref)
        handlers[id] = action
        return true
    }
}
