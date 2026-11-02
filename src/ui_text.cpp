#include "ui_text.h"

#include <atomic>
#include <unordered_map>

#include "language_setting.h"

namespace eck {

extern const char kPtBrUiTextTsv[];

namespace {

std::atomic<bool> g_portuguese_brazil{true};

bool Decode(std::string_view escaped, std::string& out) {
  out.clear();
  out.reserve(escaped.size());
  for (size_t i = 0; i < escaped.size(); ++i) {
    if (escaped[i] != '\\') {
      out.push_back(escaped[i]);
      continue;
    }
    if (++i == escaped.size()) {
      return false;
    }
    switch (escaped[i]) {
      case 'n':
        out.push_back('\n');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '\\':
        out.push_back('\\');
        break;
      default:
        return false;
    }
  }
  return true;
}

const std::unordered_map<std::string_view, const std::string*>& PortugueseBrazilIndex() {
  static const auto index = [] {
    std::unordered_map<std::string_view, const std::string*> map;
    for (const UiTextRow& row : PortugueseBrazilUiText().rows) {
      map.emplace(row.english, &row.text);
    }
    return map;
  }();
  return index;
}

}  // namespace

void SetActiveUiLanguage(std::string_view key) {
  g_portuguese_brazil = key == kPortugueseBrazilKey;
}

bool PortugueseBrazilUiSelected() {
  return g_portuguese_brazil;
}

UiTextTable ParseUiTextTable(std::string_view tsv) {
  UiTextTable table;
  size_t start = 0;
  uint32_t number = 0;
  while (start < tsv.size()) {
    size_t end = tsv.find('\n', start);
    if (end == std::string_view::npos) {
      end = tsv.size();
    }
    const std::string_view line = tsv.substr(start, end - start);
    start = end + 1;
    ++number;
    if (line.empty()) {
      continue;
    }
    const size_t tab = line.find('\t');
    UiTextRow row;
    if (tab == std::string_view::npos || line.find('\t', tab + 1) != std::string_view::npos ||
        !Decode(line.substr(0, tab), row.english) || !Decode(line.substr(tab + 1), row.text)) {
      table.errors.push_back("row " + std::to_string(number) + ": expected english and text columns");
      continue;
    }
    table.rows.push_back(std::move(row));
  }
  return table;
}

const UiTextTable& PortugueseBrazilUiText() {
  static const UiTextTable table = ParseUiTextTable(kPtBrUiTextTsv);
  return table;
}

const char* Tr(const char* english) {
  if (english == nullptr || !PortugueseBrazilUiSelected()) {
    return english;
  }
  const auto& index = PortugueseBrazilIndex();
  const auto found = index.find(english);
  return found == index.end() ? english : found->second->c_str();
}

std::string Tr(const std::string& english) {
  if (!PortugueseBrazilUiSelected()) {
    return english;
  }
  const auto& index = PortugueseBrazilIndex();
  const auto found = index.find(english);
  return found == index.end() ? english : *found->second;
}

}  // namespace eck
