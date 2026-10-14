import UserNotifications

// Discreet native notices (Notification Center), e.g. glasses plugged in or out.
@available(macOS 14.0, *)
final class Notifier: NSObject, UNUserNotificationCenterDelegate {
    private let center = UNUserNotificationCenter.current()

    override init() {
        super.init()
        center.delegate = self
        center.requestAuthorization(options: [.alert]) { _, _ in }
    }

    func post(_ title: String, _ body: String = "") {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        // Same identifier: a new notice replaces the previous one instead of piling up.
        center.add(UNNotificationRequest(identifier: "nimbo.status", content: content, trigger: nil))
    }

    // A menu bar app is often frontmost (panel open): show the banner anyway.
    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner])
    }
}
