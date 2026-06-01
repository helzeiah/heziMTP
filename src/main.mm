#include "ui/App.hpp"
#include "ui/WebUI.hpp"
#import <Cocoa/Cocoa.h>

int main(int, char**) {
    @autoreleasepool {
        heziMTP::App app;
        app.init();
        heziMTP::WebUI ui(app);
        ui.run();
        app.shutdown();
    }
    return 0;
}
