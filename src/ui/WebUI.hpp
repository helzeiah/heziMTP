#pragma once
#include "App.hpp"

namespace heziMTP {

class WebUI {
public:
    explicit WebUI(App& app);
    ~WebUI();
    void run();
private:
    App& app_;
};

} // namespace heziMTP
