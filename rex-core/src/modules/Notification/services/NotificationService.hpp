#pragma once
#include "../interfaces/INotificationService.hpp"
#include "NotificationManager.hpp"
#include "../renderer/INotificationRenderer.hpp"

namespace rex::modules { class EventBus; }
namespace rex::settings { class ISettingsService; }

namespace rex::notifications {

class NotificationService : public INotificationService {
public:
    explicit NotificationService(rex::modules::EventBus& bus,
                                 std::shared_ptr<rex::settings::ISettingsService> settingsService = nullptr);

    std::string show(const Notification& n) override;
    bool update(const std::string& id, const Notification& updated) override;
    void dismiss(const std::string& id) override;
    void dismissAll() override;
    std::vector<Notification> getDisplayed() override;
    std::optional<Notification> getById(const std::string& id) override;

    void invokeAction(const std::string& notificationId, const std::string& actionId) override;
    std::vector<Notification> getHistory(const std::string& moduleFilter = "",
                                         NotificationType typeFilter = NotificationType::Custom) override;
    void clearHistory() override;

    void setRenderer(std::unique_ptr<INotificationRenderer> renderer);

    void update(float deltaTime);

    NotificationManager& manager() { return manager_; }

private:
    NotificationManager manager_;
    std::shared_ptr<rex::settings::ISettingsService> settings_;
};

}
