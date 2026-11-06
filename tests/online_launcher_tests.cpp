#include <cstring>
#include <iterator>
#include <map>
#include <string>

#include "connection_test_gate.h"
#include "online_setting.h"
#include "online_text.h"
#include "ping_display.h"
#include "online_test_support.h"
#include "ui_text.h"

using online_test::Check;

namespace {

// The part of trg::SettingsStore the gate uses.
class FakeStore {
 public:
  std::string Get(std::string_view key, std::string_view def = {}) const {
    const auto it = values.find(std::string(key));
    return it == values.end() ? std::string(def) : it->second;
  }
  bool GetBool(std::string_view key, bool def) const {
    const std::string value = Get(key);
    return value.empty() ? def : value == "true";
  }
  void Set(std::string_view key, std::string value) { values[std::string(key)] = std::move(value); }
  std::map<std::string, std::string> values;
};

void TestConnectionGate() {
  FakeStore store;
  Check(!eck::ConnectionTestNeeded(store, false), "no test is needed with Online play off");
  store.Set(eck::kOnlineEnabledSetting, "true");
  Check(eck::ConnectionTestNeeded(store, false), "a test is needed once Online play is on");
  Check(eck::ConnectionTestNeeded(store, true), "and while one runs");
  std::string saved_key;
  const auto save = [&](std::string_view key, const std::string& value) {
    saved_key = std::string(key) + "=" + value;
    return true;
  };
  Check(eck::RecordConnectionTest(store, save), "a finished test is recorded");
  Check(saved_key == "eck_online_connection_test=" + eck::OnlineUdpConfiguration(), "and written at once");
  Check(!eck::ConnectionTestNeeded(store, false), "PLAY no longer waits");
  Check(eck::ConnectionTestNeeded(store, false, "udp 1-2"), "another UDP configuration asks again");
  store.Set(eck::kOnlineEnabledSetting, "false");
  eck::ForgetConnectionTestWhenOff(store);
  Check(store.Get(eck::kConnectionTestSetting).empty(), "turning Online play off forgets the test");
  FakeStore off;
  Check(!eck::RecordConnectionTest(off, save) && off.Get(eck::kConnectionTestSetting).empty(), "nothing is recorded with Online play off");
}

void TestSettingKeys() {
  Check(std::strcmp(eck::kOnlineEnabledSetting, "eck_online_enabled") == 0, "enabled key");
  Check(std::strcmp(eck::kOnlineNameSetting, "eck_online_name") == 0, "name key");
  Check(std::strcmp(eck::kOnlineServiceEnv, "ECK_LOBBY") == 0, "service environment variable");
  Check(eck::online::kFirstGamePort == 7101 && eck::online::kLastGamePort == 7110, "gameplay ports are clear of 7001 and 7002");
  Check(eck::online::GamePortRange().size() == 10, "ten gameplay ports");
}

void TestPingText() {
  Check(eck::PingText({}) == "Ping: --", "no peers");
  Check(eck::PingText({38, 51, 44}) == "Ping: 38 / 51 / 44 ms", "three peers");
  Check(eck::PingText({38, std::nullopt}) == "Ping: 38 / -- ms", "an unmeasured peer");
  eck::online::PeerSnapshot a, b, c;
  a.seat = 0;
  a.connected = true;
  a.rtt_ms = 20;
  b.seat = 2;
  b.connected = false;
  b.rtt_ms = 90;
  c.seat = 3;
  c.connected = true;
  Check(eck::PingText(eck::MeasuredRtts({a, b, c})) == "Ping: 20 / -- ms", "a reconnecting peer is left out");
  Check(eck::PingVisible(true, true) && !eck::PingVisible(false, true) && !eck::PingVisible(true, false), "visibility");
}

void TestLocalization() {
  const eck::TextPackage packages[] = {eck::TextPackage::kInt, eck::TextPackage::kEsm, eck::TextPackage::kEsn, eck::TextPackage::kPtb};
  Check(std::size(eck::kOnlineStrings) == 22, "all 22 online strings are listed");
  for (const eck::OnlineString& row : eck::kOnlineStrings) {
    for (const eck::TextPackage package : packages) {
      const char* text = eck::OnlineText(row.key, package);
      Check(text && *text, std::string("every package has ") + row.key);
    }
    Check(std::strcmp(eck::OnlineText(row.key, eck::TextPackage::kPtb), row.ptb) == 0, "ptb lookup");
  }
  Check(std::strcmp(eck::OnlineText("Invite Player", eck::TextPackage::kInt), "Invite Player") == 0, "INT is the English text");
  Check(std::strcmp(eck::OnlineText("Invite Player", eck::TextPackage::kEsm), "Invitar jugador") == 0, "ESM");
  Check(std::strcmp(eck::OnlineText("Player left", eck::TextPackage::kEsn), "Un jugador se ha marchado") == 0, "ESN differs from ESM");
  Check(std::strcmp(eck::OnlineText("Invite Player", eck::TextPackage::kPtb), "Convidar jogador") == 0, "PTB");
  Check(std::strcmp(eck::OnlineText("Unknown text", eck::TextPackage::kPtb), "Unknown text") == 0, "an unknown key is unchanged");
  Check(eck::PackageForLanguage("pt-BR") == eck::TextPackage::kPtb && eck::PackageForLanguage("es-MX") == eck::TextPackage::kEsm &&
            eck::PackageForLanguage("es-ES") == eck::TextPackage::kEsn && eck::PackageForLanguage("en-US") == eck::TextPackage::kInt,
        "language to package");

  // The launcher keeps its single pt-BR table; the new strings are in it.
  eck::SetActiveUiLanguage("pt-BR");
  Check(std::strcmp(eck::Tr("Connection test"), "Teste de conexão") == 0, "launcher string in pt-BR");
  Check(std::strcmp(eck::Tr("Friend requests"), "Pedidos de amizade") == 0, "friend requests in pt-BR");
  for (const eck::OnlineString& row : eck::kOnlineStrings) {
    Check(std::strcmp(eck::Tr(row.key), row.key) != 0 || std::strcmp(row.key, row.ptb) == 0 || std::strcmp(row.key, "Copy") == 0,
          std::string("launcher table translates ") + row.key);
  }
  eck::SetActiveUiLanguage("es-MX");
  Check(std::strcmp(eck::Tr("Connection test"), "Connection test") == 0, "other languages keep the launcher in English");
}

}  // namespace

int main() {
  TestConnectionGate();
  TestSettingKeys();
  TestPingText();
  TestLocalization();
  return online_test::Finish();
}
