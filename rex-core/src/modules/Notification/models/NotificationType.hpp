#pragma once

namespace rex::notifications {
enum class NotificationType {
    Information,
    Success,
    Warning,
    Error,
    Progress,
    Download,
    Update,
    Reminder,
    System,
    Custom
};
}
