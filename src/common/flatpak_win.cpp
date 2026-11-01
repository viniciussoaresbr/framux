// flatpak.h no Windows: não existe Steam Flatpak; tudo responde "ausente".
#include "common/flatpak.h"

namespace framux::flatpak {

std::string steam_home() { return {}; }
std::string bundle_dir() { return {}; }
std::string overrides_path() { return {}; }
bool steam_installed() { return false; }
Status status(bool) { return {}; }
bool enable(bool, std::string *err) {
  if (err) *err = "Steam Flatpak não existe no Windows";
  return false;
}
bool disable(std::string *) { return true; }
bool sync(std::string *) { return true; }
bool set_auto(bool, std::string *) { return true; }
std::string run_command() { return {}; }

}  // namespace framux::flatpak
