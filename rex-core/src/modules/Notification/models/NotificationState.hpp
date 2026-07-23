#pragma once

namespace rex::notifications {
enum class NotificationState {
    Pending,
    Queued,
    Displaying,
    Paused,
    Dismissed,
    Destroyed
};
}
