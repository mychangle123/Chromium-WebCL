// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "chrome/browser/extensions/startup_helper.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/base/in_process_browser_test.h"

class StartupHelperBrowserTest : public InProcessBrowserTest {
 public:
  StartupHelperBrowserTest() {}
  virtual ~StartupHelperBrowserTest() {}

  virtual void SetUpCommandLine(CommandLine* command_line) OVERRIDE {
    command_line->AppendSwitch(switches::kNoStartupWindow);
    PathService::Get(chrome::DIR_TEST_DATA, &test_data_dir_);
    test_data_dir_ = test_data_dir_.AppendASCII("extensions");
  }

 protected:
  base::FilePath test_data_dir_;
};

IN_PROC_BROWSER_TEST_F(StartupHelperBrowserTest, ValidateCrx) {
  // A list of crx file paths along with an expected result of valid (true) or
  // invalid (false).
  std::vector<std::pair<base::FilePath, bool> > expectations;
  expectations.push_back(
      std::make_pair(test_data_dir_.AppendASCII("good.crx"), true));
  expectations.push_back(
      std::make_pair(test_data_dir_.AppendASCII("good2.crx"), true));
  expectations.push_back(
      std::make_pair(test_data_dir_.AppendASCII("bad_magic.crx"), false));
  expectations.push_back(
      std::make_pair(test_data_dir_.AppendASCII("bad_underscore.crx"), false));

  for (std::vector<std::pair<base::FilePath, bool> >::iterator i =
           expectations.begin();
       i != expectations.end(); ++i) {
    CommandLine command_line(CommandLine::NO_PROGRAM);
    const base::FilePath& path = i->first;
    command_line.AppendSwitchPath(switches::kValidateCrx, path);

    std::string error;
    extensions::StartupHelper helper;
    bool result = helper.ValidateCrx(command_line, &error);
    if (i->second) {
      EXPECT_TRUE(result) << path.LossyDisplayName()
                          << " expected to be valid but wasn't";
    } else {
      EXPECT_FALSE(result) << path.LossyDisplayName()
                           << " expected to be invalid but wasn't";
      EXPECT_FALSE(error.empty()) << "Error message wasn't set for "
                                  << path.LossyDisplayName();
    }
  }
}