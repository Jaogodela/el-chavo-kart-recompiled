#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "connection_test_gate.h"
#include "online_setting.h"
#include "online_text.h"
#include "ping_display.h"
#include "online_test_support.h"
#include "ui_text.h"

using online_test::Check;

namespace {

std::string Source(const std::string& name) {
  std::ifstream file(std::string(ECK_SRC_DIR) + "/" + name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

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
  Check(eck::PingText({std::nullopt}) == "Ping: --" && eck::PingText({std::nullopt, std::nullopt}) == "Ping: --",
        "nothing measured reads --");
  Check(eck::PingText({42}) == "Ping: 42 ms", "one peer reads like SSF2");
  Check(eck::PingText({8}) == "Ping: 8 ms" && eck::PingText({113}) == "Ping: 113 ms", "whole milliseconds");
}

eck::online::PeerSnapshot Peer(int seat, bool connected, eck::online::PeerPath path, std::optional<int> rtt) {
  eck::online::PeerSnapshot peer;
  peer.seat = seat;
  peer.connected = connected;
  peer.path = path;
  peer.rtt_ms = rtt;
  return peer;
}

void TestPingVisibility() {
  Check(!eck::PingVisible(false, true), "hidden with Online play off");
  Check(!eck::PingVisible(false, false), "hidden with both off");
  Check(!eck::PingVisible(true, false), "the setting Off hides it");
  Check(eck::PingVisible(true, true), "shown with Online play on and the setting On");
  Check(eck::PingText(eck::MeasuredRtts({})) == "Ping: --", "no peer reads --");
  Check(eck::PingText(eck::MeasuredRtts({Peer(1, true, eck::online::PeerPath::kDirect, std::nullopt)})) == "Ping: --",
        "a connected peer with no round trip yet reads --");
  Check(eck::PingText(eck::MeasuredRtts({Peer(1, true, eck::online::PeerPath::kDirect, 42)})) == "Ping: 42 ms",
        "a connected peer with 42 ms reads 42 ms");
  Check(eck::PingText(eck::MeasuredRtts({Peer(1, true, eck::online::PeerPath::kRelay, 87)})) == "Ping: 87 ms",
        "a relayed peer's round trip is shown");
  Check(eck::PingText(eck::MeasuredRtts({Peer(1, false, eck::online::PeerPath::kDirect, 42)})) == "Ping: --",
        "a peer that is reconnecting is left out");
}

void TestPingSetting() {
  Check(std::strcmp(eck::kOnlinePingSetting, "eck_online_ping_display") == 0, "setting key");
  Check(!eck::kDefaultOnlinePing, "Off by default");
  Check(std::strcmp(eck::kOnlinePingLabel, "Ping display") == 0, "launcher label");
  Check(std::strcmp(eck::kOnlinePingHelp,
                    "Shows the round trip to the other player in game. F3 switches it while playing.") == 0,
        "English help");
  eck::SetActiveUiLanguage("pt-BR");
  Check(std::strcmp(eck::Tr(eck::kOnlinePingLabel), "Exibição do ping") == 0, "pt-BR label");
  Check(std::strcmp(eck::Tr(eck::kOnlinePingHelp),
                    "Mostra, durante o jogo, o tempo de ida e volta até o outro jogador. F3 liga ou desliga enquanto "
                    "você joga.") == 0,
        "pt-BR help");
  eck::SetActiveUiLanguage("en-US");
  Check(std::strcmp(eck::Tr(eck::kOnlinePingLabel), "Ping display") == 0, "English keeps the label");

  const std::string page = Source("online_page.h");
  Check(Contains(page, "OnlineSection::kConnectionTest, OnlineSection::kPingDisplay,\n"
                       "                                      OnlineSection::kFriends}"),
        "the Online page puts Ping display right after Connection test and before Friends");
  Check(Contains(page, "if (!online_on) return {OnlineSection::kOnlinePlay};"), "Online play Off still shows only its switch");

  const std::string settings = Source("settings.cpp");
  Check(Contains(settings, "REXCVAR_DEFINE_BOOL(eck_online_ping_display, false"), "the cvar defaults to Off");
  Check(Contains(settings, "\"eck_online_ping_display\",\n"), "the launcher saves it with the other online options");
  Check(Contains(Source("online_page.cpp"), "kOnlinePingLabel, kOnlinePingHelp, kOnlinePingSetting, kDefaultOnlinePing"),
        "the Online page draws the toggle");
  Check(Contains(Source("online_page.cpp"), "kDefaultOnlinePing, \"Off\", \"On\""), "as Off / On");
}

bool ContainsWord(const std::string& text, std::string_view word) {
  for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
    const bool before = at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1]));
    const bool after = at + word.size() >= text.size() || !std::isalnum(static_cast<unsigned char>(text[at + word.size()]));
    if (before && after) return true;
  }
  return false;
}

void TestPingAddsNoPackets() {
  for (const char* file : {"online/game_socket.cpp", "online/relay_frame.h", "online_race.h", "online_race.cpp",
                           "online_race_sync.h", "online_race_sync_guest.cpp"}) {
    const std::string text = Source(file);
    Check(!text.empty(), std::string("read ") + file);
    Check(!ContainsWord(text, "ping") && !ContainsWord(text, "Ping") && !ContainsWord(text, "pong"),
          std::string(file) + " has no ping or pong packet");
  }
  for (const char* file : {"ping_display.h", "ping_display.cpp"}) {
    const std::string text = Source(file);
    Check(!Contains(text, "Send") && !Contains(text, "Encode") && !Contains(text, "game_socket") && !Contains(text, "lobby_link"),
          std::string(file) + " sends nothing");
  }
  const std::string display = Source("ping_display.cpp");
  Check(Contains(display, "Online().snapshot()"), "the panel reads the service snapshot");
  Check(!Contains(display, "lobby_latency") && !Contains(display, "connection_test") && !Contains(display, "stun"),
        "and no connection test or lobby latency");
  Check(Contains(Source("online_menu_overlay.cpp"), "Online().SetPeerRtt(peer.seat, race.rtt_ms[size_t(peer.seat)])"),
        "the snapshot's round trip is the race sync's");
  Check(Contains(Source("online_race_sync_guest.cpp"), "running->sync.RttMs(seat, now)"), "measured from the sync acknowledgements");
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
  TestPingVisibility();
  TestPingSetting();
  TestPingAddsNoPackets();
  TestLocalization();
  return online_test::Finish();
}
