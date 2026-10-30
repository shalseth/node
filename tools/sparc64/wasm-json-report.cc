// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
#include <nlohmann/json.hpp>
#include <cstdint>
#ifdef SCALAR_APP_NATIVE
#include <iostream>
#endif
#include <string>

namespace {
std::string& output() {
  static std::string value;
  return value;
}
int caught = 0;
}
extern "C" int process(const char* data, int length, int mode) {
  using nlohmann::json;
  try {
    auto document = json::parse(data, data + length);
    json result;
    if (mode == 0) {
      result = document;
    } else if (mode == 1) {
      std::map<std::string, int64_t> totals;
      for (const auto& item : document.at("entries")) {
        const auto user = item.at("user").get<std::string>();
        totals[user] += item.at("amount").get<int64_t>();
      }
      result = json{{"count", document.at("entries").size()}, {"totals", totals}};
    } else if (mode == 2) {
      result = json{{"name", document.at("name").get<std::string>()},
                    {"enabled", document.at("enabled").get<bool>()},
                    {"ports", document.at("ports").get<std::vector<int>>()}};
    } else {
      throw std::runtime_error("unsupported report mode");
    }
    output() = json{{"ok", true}, {"result", result}}.dump();
    return 0;
  } catch (const json::exception& error) {
    ++caught;
    output() = json{{"ok", false}, {"category", "json"},
                    {"id", error.id}, {"message", error.what()}}.dump();
    return 1;
  } catch (const std::exception& error) {
    ++caught;
    output() = json{{"ok", false}, {"category", "application"},
                    {"message", error.what()}}.dump();
    return 2;
  }
}
extern "C" const char* result_data() { return output().data(); }
extern "C" int result_size() { return static_cast<int>(output().size()); }
extern "C" int caught_count() { return caught; }
#ifdef SCALAR_APP_NATIVE
int main(int argc, char** argv) {
  const int mode = argc > 1 ? std::stoi(argv[1]) : 0;
  std::string input;
  if (std::getline(std::cin, input)) {
    process(input.data(), static_cast<int>(input.size()), mode);
    std::cout << output() << '\n';
  }
}
#endif
