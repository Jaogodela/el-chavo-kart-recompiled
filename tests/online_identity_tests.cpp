#include <filesystem>
#include <fstream>
#include <sstream>

#include "online/constants.h"
#include "online/identity.h"
#include "online/protocol.h"
#include "online_test_support.h"

using online_test::Check;
namespace fs = std::filesystem;
namespace online = eck::online;

namespace {

std::string ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

int main() {
  // The same derivation as playerIdFor in server/eck-lobby/src/protocol.js.
  const std::string token(64, 'a');
  const std::string known_token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  Check(online::DerivePlayerId(known_token) == "TCSA-EF6F", "friend code of a known token matches the service");
  const std::string code = online::DerivePlayerId(token);
  Check(code.size() == 9 && code[4] == '-', "friend code is XXXX-XXXX");
  Check(code.find_first_of("IO01") == std::string::npos, "friend code avoids I, O, 0 and 1");
  Check(online::NormalizeFriendCode("tcsa ef6f") == "TCSA-EF6F", "typed code is normalized");
  Check(online::NormalizeFriendCode("TCSA-EF6") .empty(), "short code is refused");
  Check(online::NormalizeFriendCode("TCSA-EF0F").empty(), "code with a 0 is refused");

  Check(std::string(online::kGameId) == "el-chavo-kart-pc" && online::kProtocolVersion == 1, "game id and protocol");
  Check(std::string(online::kImageId) == "eck-usa-1db4e96b", "image id");

  Check(online::CleanOnlineName("Saulo Benigno!") == "SauloBenigno", "name keeps letters and digits only");
  Check(online::CleanOnlineName("a-b_c") == "a-b_c", "dash and underscore stay");
  Check(online::CleanOnlineName("12345678901234567890") == "1234567890123456", "name is cut at 16");
  Check(online::CleanOnlineName("!!!") == "Player", "empty name becomes Player");

  const fs::path dir = fs::temp_directory_path() / "eck_online_identity_test";
  fs::remove_all(dir);
  const online::Identity first = online::LoadOrCreateIdentity(dir);
  Check(first.ok() && !first.ephemeral, "identity is created");
  Check(first.token.size() == 64, "token is 32 random bytes in hex");
  Check(first.player_id == online::DerivePlayerId(first.token), "friend code derives from the token");
  Check(fs::exists(dir / "eck_online_identity.ini"), "identity file has the title-specific name");
  const online::Identity second = online::LoadOrCreateIdentity(dir);
  Check(second.token == first.token, "identity is reused");
  const std::string before = ReadAll(dir / "eck_online_identity.ini");
  { std::ofstream(dir / "eck_online_identity.ini", std::ios::binary | std::ios::trunc) << "token=zz\n"; }
  const online::Identity damaged = online::LoadOrCreateIdentity(dir);
  Check(!damaged.ok() && !damaged.error.empty(), "damaged file is reported, not replaced");
  Check(ReadAll(dir / "eck_online_identity.ini") == "token=zz\n", "damaged file is left alone");
  (void)before;
  fs::remove_all(dir);

  const online::Identity a = online::MakeEphemeralIdentity();
  const online::Identity b = online::MakeEphemeralIdentity();
  Check(a.ok() && a.ephemeral && a.token != b.token, "ephemeral identities differ");
  return online_test::Finish();
}
