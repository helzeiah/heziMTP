#include "WebUI.hpp"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <string>

// ─── Forward declarations ─────────────────────────────────────────────────────
@class HeziBridge;
@class HeziAppDelegate;

// ─── HeziBridge: JS → C++ message handler ─────────────────────────────────────
@interface HeziBridge : NSObject <WKScriptMessageHandler, WKNavigationDelegate>
- (instancetype)initWithApp:(heziMTP::App*)app webView:(WKWebView*)webView;
@end

@implementation HeziBridge {
    heziMTP::App*  _app;
    WKWebView*     _webView;
}

- (instancetype)initWithApp:(heziMTP::App*)app webView:(WKWebView*)webView {
    if ((self = [super init])) {
        _app     = app;
        _webView = webView;
    }
    return self;
}

- (void)userContentController:(WKUserContentController*)ucc
      didReceiveScriptMessage:(WKScriptMessage*)message {
    (void)ucc;
    if (![message.name isEqualToString:@"bridge"]) return;
    NSDictionary* body = message.body;
    if (![body isKindOfClass:[NSDictionary class]]) return;

    NSNumber* msgId  = body[@"id"];
    NSString* action = body[@"action"];
    if (!msgId || !action) return;

    long long rid = msgId.longLongValue;

    // All dispatched on main thread already (WKWebView guarantee), but we
    // use dispatch_async to avoid blocking the JS engine during heavy ops.
    dispatch_async(dispatch_get_main_queue(), ^{
        [self handleAction:action body:body responseId:rid];
    });
}

- (void)handleAction:(NSString*)action body:(NSDictionary*)body responseId:(long long)rid {
    NSString* result = nil;

    if ([action isEqualToString:@"get_state"]) {
        std::string json = _app->state_json();
        result = [NSString stringWithUTF8String:json.c_str()];
        // result is raw JSON object, send directly
        [self sendRawResult:result responseId:rid];
        return;

    } else if ([action isEqualToString:@"navigate_local"]) {
        NSString* path = body[@"path"];
        if (path) _app->navigate_local(path.UTF8String);

    } else if ([action isEqualToString:@"navigate_local_up"]) {
        _app->navigate_local_up();

    } else if ([action isEqualToString:@"refresh_local"]) {
        _app->navigate_local(_app->current_local_path());

    } else if ([action isEqualToString:@"navigate_remote"]) {
        NSNumber* handle    = body[@"handle"];
        NSNumber* storageId = body[@"storageId"];
        NSString* name      = body[@"name"];
        if (handle && storageId && name)
            _app->navigate_remote(handle.unsignedIntValue,
                                  storageId.unsignedIntValue,
                                  name.UTF8String);

    } else if ([action isEqualToString:@"navigate_remote_up"]) {
        _app->navigate_remote_up();

    } else if ([action isEqualToString:@"navigate_remote_to"]) {
        NSNumber* index = body[@"index"];
        if (index) _app->navigate_remote_to((size_t)index.unsignedIntegerValue);

    } else if ([action isEqualToString:@"refresh_remote"]) {
        _app->request_refresh_remote();

    } else if ([action isEqualToString:@"set_storage"]) {
        NSNumber* storageId = body[@"storageId"];
        if (storageId) _app->set_active_storage(storageId.unsignedIntValue);

    } else if ([action isEqualToString:@"start_upload"]) {
        NSString* srcPath = body[@"srcPath"];
        if (srcPath && srcPath.length > 0) _app->do_upload(srcPath.UTF8String);

    } else if ([action isEqualToString:@"start_download"]) {
        NSNumber* handle    = body[@"handle"];
        NSNumber* storageId = body[@"storageId"];
        NSString* filename  = body[@"filename"];
        NSNumber* size      = body[@"size"];
        if (handle && storageId && filename && size)
            _app->do_download(handle.unsignedIntValue,
                              storageId.unsignedIntValue,
                              filename.UTF8String,
                              size.unsignedLongLongValue);

    } else if ([action isEqualToString:@"cancel_transfer"]) {
        NSString* tid = body[@"id"];
        if (tid) _app->do_cancel(tid.UTF8String);

    } else if ([action isEqualToString:@"cancel_all"]) {
        _app->do_cancel_all();

    } else if ([action isEqualToString:@"delete_remote"]) {
        NSNumber* handle = body[@"handle"];
        if (handle) _app->do_delete(handle.unsignedIntValue);

    } else if ([action isEqualToString:@"create_folder"]) {
        NSString* name = body[@"name"];
        if (name && name.length > 0) _app->do_create_folder(name.UTF8String);

    } else if ([action isEqualToString:@"reconnect"]) {
        _app->reconnect();

    } else if ([action isEqualToString:@"open_file_picker"]) {
        // Open NSOpenPanel to pick files for upload
        dispatch_async(dispatch_get_main_queue(), ^{
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = YES;
            [panel beginWithCompletionHandler:^(NSModalResponse r) {
                if (r == NSModalResponseOK) {
                    for (NSURL* url in panel.URLs) {
                        NSString* path = url.path;
                        if (path) self->_app->do_upload(path.UTF8String);
                    }
                }
            }];
        });
    }

    // Send null result for void actions
    [self sendNullResult:rid];
}

- (void)sendRawResult:(NSString*)json responseId:(long long)rid {
    NSString* js = [NSString stringWithFormat:@"window.bridgeResponse(%lld,%@)", rid, json];
    [_webView evaluateJavaScript:js completionHandler:nil];
}

- (void)sendNullResult:(long long)rid {
    NSString* js = [NSString stringWithFormat:@"window.bridgeResponse(%lld,null)", rid];
    [_webView evaluateJavaScript:js completionHandler:nil];
}

// WKNavigationDelegate — log errors
- (void)webView:(WKWebView*)wv didFailNavigation:(WKNavigation*)nav withError:(NSError*)err {
    (void)wv; (void)nav;
    NSLog(@"[heziMTP] WebView navigation failed: %@", err.localizedDescription);
}
- (void)webView:(WKWebView*)wv didFailProvisionalNavigation:(WKNavigation*)nav withError:(NSError*)err {
    (void)wv; (void)nav;
    NSLog(@"[heziMTP] WebView provisional navigation failed: %@", err.localizedDescription);
}

@end

// ─── HeziAppDelegate ──────────────────────────────────────────────────────────
@interface HeziAppDelegate : NSObject <NSApplicationDelegate>
- (instancetype)initWithApp:(heziMTP::App*)app;
@end

@implementation HeziAppDelegate {
    heziMTP::App* _app;
    NSWindow*     _window;
    WKWebView*    _webView;
    HeziBridge*   _bridge;
}

- (instancetype)initWithApp:(heziMTP::App*)app {
    if ((self = [super init])) {
        _app = app;
    }
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    (void)note;

    // ── WKWebView configuration ───────────────────────────────────────────────
    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];

    // Enable developer tools in debug builds
#if DEBUG
    if (@available(macOS 13.3, *)) {
        config.preferences.elementFullscreenEnabled = YES;
        [config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
    }
#endif

    // Register the "bridge" message handler
    _webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 800)
                                  configuration:config];
    _bridge  = [[HeziBridge alloc] initWithApp:_app webView:_webView];
    [config.userContentController addScriptMessageHandler:_bridge name:@"bridge"];
    _webView.navigationDelegate = _bridge;

    // Allow file:// access for local resource loading
    _webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    // ── NSWindow ──────────────────────────────────────────────────────────────
    NSRect frame = NSMakeRect(0, 0, 1280, 800);
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled |
        NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable |
        NSWindowStyleMaskResizable |
        NSWindowStyleMaskFullSizeContentView;

    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title                    = @"heziMTP";
    _window.titleVisibility          = NSWindowTitleHidden;
    _window.titlebarAppearsTransparent = YES;
    _window.minSize                  = NSMakeSize(900, 600);
    _window.movableByWindowBackground = NO;

    // Force dark appearance
    if (@available(macOS 10.14, *)) {
        _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    }

    [_window setContentView:_webView];
    [_window center];
    [_window makeKeyAndOrderFront:nil];

    // ── Load index.html from bundle (dev fallback: source tree) ───────────────
    NSURL* indexURL = nil;

    // 1. Try the bundle's webroot resource directory
    NSBundle* bundle = [NSBundle mainBundle];
    indexURL = [bundle URLForResource:@"index" withExtension:@"html" subdirectory:@"webroot"];

    // 2. Dev fallback: look next to the binary in a webroot/ sibling
    if (!indexURL) {
        NSString* execDir = bundle.executableURL.URLByDeletingLastPathComponent.path;
        NSString* devPath = [execDir stringByAppendingPathComponent:@"webroot/index.html"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:devPath])
            indexURL = [NSURL fileURLWithPath:devPath];
    }

    // 3. Source tree fallback for direct cmake build without bundle
    if (!indexURL) {
        // Walk up from executable looking for src/ui/webroot
        NSString* base = bundle.executableURL.URLByDeletingLastPathComponent.path;
        NSArray<NSString*>* candidates = @[
            [base stringByAppendingPathComponent:@"../src/ui/webroot/index.html"],
            [base stringByAppendingPathComponent:@"../../src/ui/webroot/index.html"],
            [base stringByAppendingPathComponent:@"../../../src/ui/webroot/index.html"],
        ];
        for (NSString* c in candidates) {
            NSString* resolved = c.stringByResolvingSymlinksInPath;
            if ([[NSFileManager defaultManager] fileExistsAtPath:resolved]) {
                indexURL = [NSURL fileURLWithPath:resolved];
                break;
            }
        }
    }

    if (indexURL) {
        NSURL* baseURL = indexURL.URLByDeletingLastPathComponent;
        NSString* html = [NSString stringWithContentsOfURL:indexURL
                                                  encoding:NSUTF8StringEncoding
                                                     error:nil];
        if (html) {
            [_webView loadHTMLString:html baseURL:baseURL];
        } else {
            [_webView loadFileURL:indexURL allowingReadAccessToURL:baseURL];
        }
    } else {
        // Inline fallback so the app at least opens
        NSString* fallback = @"<!DOCTYPE html><html><body style='background:#1a1a1a;color:#fff;font-family:system-ui;display:flex;align-items:center;justify-content:center;height:100vh'><h2>webroot/index.html not found</h2></body></html>";
        [_webView loadHTMLString:fallback baseURL:nil];
        NSLog(@"[heziMTP] ERROR: webroot/index.html not found in bundle or source tree");
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
    (void)note;
}

@end

// ─── WebUI ────────────────────────────────────────────────────────────────────
namespace heziMTP {

WebUI::WebUI(App& a) : app_(a) {}
WebUI::~WebUI() = default;

void WebUI::run() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        HeziAppDelegate* delegate = [[HeziAppDelegate alloc] initWithApp:&app_];
        [NSApp setDelegate:delegate];

        // App menu (Cmd+Q to quit)
        NSMenu* menuBar  = [[NSMenu alloc] init];
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [menuBar addItem:appItem];
        NSMenu* appMenu  = [[NSMenu alloc] init];
        NSMenuItem* quitItem = [[NSMenuItem alloc]
            initWithTitle:@"Quit heziMTP"
                   action:@selector(terminate:)
            keyEquivalent:@"q"];
        [appMenu addItem:quitItem];
        appItem.submenu = appMenu;
        [NSApp setMainMenu:menuBar];

        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
}

} // namespace heziMTP
