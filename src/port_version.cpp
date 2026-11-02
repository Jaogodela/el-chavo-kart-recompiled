#include "port_version.h"

namespace eck {

const std::string& PortVersion() {
  static const std::string version = std::to_string(ECK_VERSION_MAJOR) + "." + std::to_string(ECK_VERSION_MINOR) +
                                     "." + std::to_string(ECK_VERSION_PATCH);
  return version;
}

}  // namespace eck
