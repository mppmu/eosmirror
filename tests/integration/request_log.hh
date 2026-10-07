// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClLog.hh>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// The requests XrdCl sends, from its log at the dump level, which has a line
// "[host:port #n] Sending message kXR_... (...) through substream ..." for
// each. The log output is taken over once, before XrdCl starts any threads;
// other messages still go to stderr.
class RequestLog : public XrdCl::LogOut {
 public:
  // Call before anything else uses XrdCl.
  static void install() {
    instance_ = new RequestLog;
    XrdCl::DefaultEnv::GetLog()->SetOutput(instance_);  // which owns it
  }

  // Records the requests sent until stop().
  static void start() {
    {
      std::lock_guard lock(instance_->mutex_);
      instance_->sent_.clear();
      instance_->recording_ = true;
    }
    XrdCl::Log* log = XrdCl::DefaultEnv::GetLog();
    instance_->level_ = log->GetLevel();
    log->SetLevel(XrdCl::Log::DumpMsg);
  }

  // The lines of the requests sent since start().
  static std::vector<std::string> stop() {
    XrdCl::DefaultEnv::GetLog()->SetLevel(instance_->level_);
    std::lock_guard lock(instance_->mutex_);
    instance_->recording_ = false;
    return std::move(instance_->sent_);
  }

  // How many of the requests went to the host (or, with to_host false, to
  // other hosts) and were of the given kind ("kXR_open").
  static size_t count(const std::vector<std::string>& sent, std::string_view host,
                      std::string_view request, bool to_host = true) {
    std::string server = "[" + std::string(host) + ":";
    std::string message = "Sending message " + std::string(request) + " ";
    return static_cast<size_t>(std::count_if(sent.begin(), sent.end(), [&](const std::string& s) {
      return (s.find(server) != std::string::npos) == to_host &&
             s.find(message) != std::string::npos;
    }));
  }

  void Write(const std::string& message) override {
    std::lock_guard lock(mutex_);
    if (!recording_) {
      std::fputs(message.c_str(), stderr);
      return;
    }
    if (message.find("Sending message ") != std::string::npos &&
        message.find(" through substream ") != std::string::npos)
      sent_.push_back(message);
  }

 private:
  static inline RequestLog* instance_ = nullptr;
  std::mutex mutex_;
  bool recording_ = false;
  std::vector<std::string> sent_;
  XrdCl::Log::LogLevel level_ = XrdCl::Log::ErrorMsg;
};
