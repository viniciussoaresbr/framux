#include "core/arbiter.h"

namespace framux {

uint64_t Arbiter::update(const std::vector<SessionView> &sessions, uint64_t now) {
  const SessionView *cur = nullptr;
  for (const auto &s : sessions)
    if (s.id == active_) cur = &s;
  if (cur && cur->busy) return active_;

  auto fresh = [now](const SessionView &s) { return s.last_present_ns != 0 && now - s.last_present_ns < kStaleNs; };

  const SessionView *focused = nullptr;
  const SessionView *recent = nullptr;
  for (const auto &s : sessions) {
    if (s.focus == 1 && fresh(s) && (!focused || s.last_present_ns > focused->last_present_ns)) focused = &s;
    if (fresh(s) && (!recent || s.last_present_ns > recent->last_present_ns)) recent = &s;
  }
  if (focused)
    active_ = focused->id;
  else if (!cur)
    active_ = recent ? recent->id : (sessions.empty() ? 0 : sessions.back().id);
  else if (!fresh(*cur) && recent)
    active_ = recent->id;
  return active_;
}

}  // namespace framux
