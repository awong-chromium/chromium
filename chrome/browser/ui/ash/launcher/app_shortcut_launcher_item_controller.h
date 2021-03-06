// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_ASH_LAUNCHER_APP_SHORTCUT_LAUNCHER_ITEM_CONTROLLER_H_
#define CHROME_BROWSER_UI_ASH_LAUNCHER_APP_SHORTCUT_LAUNCHER_ITEM_CONTROLLER_H_

#include <string>

#include "chrome/browser/ui/ash/launcher/chrome_launcher_controller_per_app.h"
#include "chrome/browser/ui/ash/launcher/launcher_item_controller.h"

namespace aura {
class Window;
}

namespace extensions {
class Extension;
}

class ChromeLauncherController;

// Item controller for an app shortcut. Shortcuts track app and launcher ids,
// but do not have any associated windows (opening a shortcut will replace the
// item with the appropriate LauncherItemController type).
class AppShortcutLauncherItemController : public LauncherItemController {
 public:
  AppShortcutLauncherItemController(const std::string& app_id,
                                    ChromeLauncherControllerPerApp* controller);

  virtual ~AppShortcutLauncherItemController();

  // LauncherItemController overrides:
  virtual string16 GetTitle() OVERRIDE;
  virtual bool HasWindow(aura::Window* window) const OVERRIDE;
  virtual bool IsOpen() const OVERRIDE;
  virtual void Launch(int event_flags) OVERRIDE;
  virtual void Activate() OVERRIDE;
  virtual void Close() OVERRIDE;
  virtual void Clicked(const ui::Event& event) OVERRIDE;
  virtual void OnRemoved() OVERRIDE;
  virtual void LauncherItemChanged(
      int model_index,
      const ash::LauncherItem& old_item) OVERRIDE;
  virtual ChromeLauncherAppMenuItems GetApplicationList() OVERRIDE;
  std::vector<content::WebContents*> GetRunningApplications();

  // Get the refocus url pattern, which can be used to identify this application
  // from a URL link.
  const GURL& refocus_url() const { return refocus_url_; }
  // Set the refocus url pattern. Used by unit tests.
  void set_refocus_url(const GURL& refocus_url) { refocus_url_ = refocus_url; }

 private:
  // Get the last running application.
  content::WebContents* GetLRUApplication();

  // Returns true if this app matches the given |web_contents|. To accelerate
  // the matching, the app managing |extension| as well as the parsed
  // |refocus_pattern| get passed.
  bool WebContentMatchesApp(const extensions::Extension* extension,
                            const URLPattern& refocus_pattern,
                            content::WebContents* web_contents);

  GURL refocus_url_;
  ChromeLauncherControllerPerApp* app_controller_;

  DISALLOW_COPY_AND_ASSIGN(AppShortcutLauncherItemController);
};

#endif  // CHROME_BROWSER_UI_ASH_LAUNCHER_APP_SHORTCUT_LAUNCHER_ITEM_CONTROLLER_H_
