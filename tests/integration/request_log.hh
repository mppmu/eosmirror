// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClLog.hh>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The requests XrdCl sends, from its log at the dump level, which has a line
// "[host:port #n] Sending message kXR_... (...) through substream ..." for
// each. The log output is taken over once, before XrdCl starts any threads.
// Outside a recording, every message goes to stderr; during one, only the
// request lines are kept.
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

  // The lines of the requests sent since start(), with those in which XrdCl
  // announces a request it sends again.
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

  // The requests in the order they were sent, each as "<to> <kind>[ <detail>]":
  // to is "server" for the host and "node" for any other (where EOS
  // redirects to its storage nodes). The detail tells requests of a kind
  // apart without the names and numbers that differ between runs: how an
  // open accesses the file and the eos.atomic and eos.mtime parameters it
  // carries ("proc" for an MGM command), offset and size of a read or write
  // (but not of the reads of an MGM command's reply), the code of a query, a
  // mode, and "temp" where a path is an eosmirror temporary. The requests that set up
  // connections (logins and the like) are left out, and so are those XrdCl
  // sends again because the server told it to wait (kXR_wait), which depends
  // on the server's load.
  static std::vector<std::string> sequence(const std::vector<std::string>& sent,
                                           std::string_view host) {
    std::vector<std::string> result;
    // The host of the last request of each kind, and the kinds that XrdCl
    // is about to send again to the same host.
    std::map<std::string, std::string, std::less<>> last_host;
    std::set<std::string, std::less<>> resending;
    // The host whose MGM command is being read, from its open to its close.
    std::string in_proc;
    auto host_of = [](std::string_view s, size_t before) {
      size_t bracket = s.rfind('[', before);
      if (bracket == std::string_view::npos) return std::string_view();
      std::string_view h = s.substr(bracket + 1);
      return h.substr(0, h.find(':'));
    };
    for (const std::string& s : sent) {
      // XrdCl announces a request it sends again, after a wait or to the
      // host of a redirection, as "[host:port] Retry at server MsgHandler:
      // 0x... (message: kXR_... (...".
      constexpr std::string_view kRetry = "Retry at server MsgHandler: ";
      constexpr std::string_view kMessage = "(message: ";
      if (size_t retry = s.find(kRetry); retry != std::string::npos) {
        size_t m = s.find(kMessage, retry);
        if (m == std::string::npos) continue;
        std::string kind = s.substr(m + kMessage.size());
        kind = kind.substr(0, kind.find(' '));
        auto last = last_host.find(kind);
        if (last != last_host.end() && last->second == host_of(s, retry)) resending.insert(kind);
        continue;
      }
      constexpr std::string_view kSending = "Sending message ";
      size_t at = s.find(kSending);
      if (at == std::string::npos) continue;
      std::string_view to_host = host_of(s, at);
      std::string_view rest = std::string_view(s).substr(at + kSending.size());
      std::string kind(rest.substr(0, rest.find(' ')));
      last_host[kind] = std::string(to_host);
      if (resending.erase(kind)) continue;
      if (kind == "kXR_protocol" || kind == "kXR_login" || kind == "kXR_auth" ||
          kind == "kXR_endsess" || kind == "kXR_ping" || kind == "kXR_bind" || kind == "kXR_sigver")
        continue;
      // The request's own parameters, up to the message address that follows.
      std::string_view params = rest.substr(kind.size());
      params = params.substr(0, params.find(") (0x"));
      bool node = to_host != host;
      auto has = [&](std::string_view word) { return params.find(word) != std::string_view::npos; };
      auto field = [&](std::string_view name) {
        size_t f = params.find(name);
        if (f == std::string_view::npos) return std::string();
        std::string_view v = params.substr(f + name.size());
        return std::string(v.substr(0, v.find_first_of(",)")));
      };
      std::string line = (node ? "node " : "server ") + kind;
      auto add = [&](std::string_view word) { line.append(" ").append(word); };
      if (kind == "kXR_open") {
        if (has("file: /proc/")) {
          add("proc");
          in_proc = to_host;
        } else {
          // XRootD 5.6 does not print kXR_open_wrto.
          add(has("kXR_open_read") ? "read" : has("kXR_open_updt") ? "update" : "write");
          if (has("kXR_new")) add("new");
          if (has("kXR_delete")) add("delete");
          if (has("eos.atomic=1")) add("atomic");
          if (has("eos.mtime=")) add("mtime");
        }
      } else if ((kind == "kXR_read" || kind == "kXR_write") && in_proc != to_host) {
        add(field("offset: ") + "+" + field("size: "));
      } else if (kind == "kXR_close" && in_proc == to_host) {
        in_proc.clear();
      } else if (kind == "kXR_stat") {
        if (has("(handle:")) add("open file");
      } else if (kind == "kXR_query") {
        add(field("code: "));
      } else if (kind == "kXR_mkdir" || kind == "kXR_chmod") {
        add("mode " + field("mode: "));
      }
      if (has(".eosmirror-")) add("temp");
      result.push_back(std::move(line));
    }
    return result;
  }

  void Write(const std::string& message) override {
    std::lock_guard lock(mutex_);
    if (!recording_) {
      std::fputs(message.c_str(), stderr);
      return;
    }
    if ((message.find("Sending message ") != std::string::npos &&
         message.find(" through substream ") != std::string::npos) ||
        message.find("Retry at server MsgHandler: ") != std::string::npos)
      sent_.push_back(message);
  }

 private:
  static inline RequestLog* instance_ = nullptr;
  std::mutex mutex_;
  bool recording_ = false;
  std::vector<std::string> sent_;
  XrdCl::Log::LogLevel level_ = XrdCl::Log::ErrorMsg;
};
