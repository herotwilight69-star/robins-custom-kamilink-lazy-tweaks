#pragma once

#include <string>
#include <string_view>

namespace dusk::ui::app_update {

enum class State {
    Idle,
    Downloading,
    Preparing,
    Ready,
    Failed,
};

bool begin(std::string_view url, std::string_view digest, std::string& error);
void update();
void show_error(std::string message);
State state() noexcept;
std::string message();

}  // namespace dusk::ui::app_update
