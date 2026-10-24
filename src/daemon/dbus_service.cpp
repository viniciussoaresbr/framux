#include "daemon/dbus_service.h"

#include <poll.h>
#include <sys/epoll.h>
#include <systemd/sd-bus.h>
#include <time.h>

#include <cerrno>

#include "common/log.h"

namespace framux::daemon {

struct DbusService::Impl {
  sd_bus *bus = nullptr;
  sd_bus_slot *slot = nullptr;
  DbusHandlers h;
};

namespace {

int reply_action(sd_bus_message *m, sd_bus_error *err, const ActionResult &r) {
  if (!r.ok) return sd_bus_error_set(err, r.error_name.c_str(), r.message.c_str());
  return sd_bus_reply_method_return(m, "s", r.message.c_str());
}

int m_status(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *d = static_cast<DbusService::Impl *>(ud);
  return sd_bus_reply_method_return(m, "s", d->h.status(false).c_str());
}

int m_status_json(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *d = static_cast<DbusService::Impl *>(ud);
  return sd_bus_reply_method_return(m, "s", d->h.status(true).c_str());
}

int m_benchmark(sd_bus_message *m, void *ud, sd_bus_error *err) {
  auto *d = static_cast<DbusService::Impl *>(ud);
  const char *action = nullptr;
  uint32_t duration = 0;
  int r = sd_bus_message_read(m, "su", &action, &duration);
  if (r < 0) return r;
  return reply_action(m, err, d->h.benchmark(action, duration));
}

int m_screenshot(sd_bus_message *m, void *ud, sd_bus_error *err) {
  auto *d = static_cast<DbusService::Impl *>(ud);
  int overlay = 0;
  int r = sd_bus_message_read(m, "b", &overlay);
  if (r < 0) return r;
  ActionResult res = d->h.screenshot(overlay != 0);
  if (!res.ok) return sd_bus_error_set(err, res.error_name.c_str(), res.message.c_str());
  return sd_bus_reply_method_return(m, "t", res.request_id);
}

int m_overlay(sd_bus_message *m, void *ud, sd_bus_error *err) {
  auto *d = static_cast<DbusService::Impl *>(ud);
  const char *action = nullptr;
  int r = sd_bus_message_read(m, "s", &action);
  if (r < 0) return r;
  return reply_action(m, err, d->h.overlay(action));
}

int m_record(sd_bus_message *m, void *ud, sd_bus_error *err) {
  auto *d = static_cast<DbusService::Impl *>(ud);
  const char *action = nullptr;
  int r = sd_bus_message_read(m, "s", &action);
  if (r < 0) return r;
  return reply_action(m, err, d->h.video("record", action));
}

int m_loop(sd_bus_message *m, void *ud, sd_bus_error *err) {
  auto *d = static_cast<DbusService::Impl *>(ud);
  const char *action = nullptr;
  int r = sd_bus_message_read(m, "s", &action);
  if (r < 0) return r;
  return reply_action(m, err, d->h.video("loop", action));
}

int m_reload(sd_bus_message *m, void *ud, sd_bus_error *) {
  static_cast<DbusService::Impl *>(ud)->h.reload();
  return sd_bus_reply_method_return(m, "");
}

const sd_bus_vtable kVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_ARGS("Status", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", text), m_status, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("StatusJson", SD_BUS_NO_ARGS, SD_BUS_RESULT("s", json), m_status_json,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("Benchmark", SD_BUS_ARGS("s", action, "u", duration_s), SD_BUS_RESULT("s", message),
                            m_benchmark, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("Screenshot", SD_BUS_ARGS("b", include_overlay), SD_BUS_RESULT("t", request_id),
                            m_screenshot, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("Overlay", SD_BUS_ARGS("s", action), SD_BUS_RESULT("s", message), m_overlay,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("Record", SD_BUS_ARGS("s", action), SD_BUS_RESULT("s", message), m_record,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("Loop", SD_BUS_ARGS("s", action), SD_BUS_RESULT("s", message), m_loop,
                            SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Reload", "", "", m_reload, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL_WITH_ARGS("StatsChanged", SD_BUS_ARGS("d", fps, "t", frames, "s", game, "u", flags), 0),
    SD_BUS_SIGNAL_WITH_ARGS("FileSaved", SD_BUS_ARGS("t", request_id, "s", kind, "s", path), 0),
    SD_BUS_SIGNAL_WITH_ARGS("Error", SD_BUS_ARGS("t", request_id, "s", message), 0),
    SD_BUS_VTABLE_END,
};

}  // namespace

DbusService::~DbusService() {
  if (!impl_) return;
  sd_bus_slot_unref(impl_->slot);
  sd_bus_flush_close_unref(impl_->bus);
}

std::unique_ptr<DbusService> DbusService::open(DbusHandlers h, bool *already_running) {
  *already_running = false;
  std::unique_ptr<DbusService> s(new DbusService());
  s->impl_ = std::make_unique<Impl>();
  s->impl_->h = std::move(h);
  int r = sd_bus_open_user(&s->impl_->bus);
  if (r < 0) {
    FRAMUX_LOG("D-Bus de sessão indisponível (%s); CLI e notificações desativadas", strerror(-r));
    s->impl_.reset();
    return nullptr;
  }
  r = sd_bus_add_object_vtable(s->impl_->bus, &s->impl_->slot, kObjectPath, kInterface, kVtable, s->impl_.get());
  if (r >= 0) r = sd_bus_request_name(s->impl_->bus, kBusName, 0);
  if (r < 0) {
    if (r == -EEXIST)
      *already_running = true;
    else
      FRAMUX_LOG("D-Bus: não consegui registrar %s: %s", kBusName, strerror(-r));
    sd_bus_slot_unref(s->impl_->slot);
    sd_bus_flush_close_unref(s->impl_->bus);
    s->impl_.reset();
    return nullptr;
  }
  return s;
}

int DbusService::fd() const { return sd_bus_get_fd(impl_->bus); }

uint32_t DbusService::events() const {
  const int e = sd_bus_get_events(impl_->bus);
  uint32_t out = 0;
  if (e & POLLIN) out |= EPOLLIN;
  if (e & POLLOUT) out |= EPOLLOUT;
  return out;
}

int DbusService::timeout_ms() const {
  uint64_t usec = 0;
  if (sd_bus_get_timeout(impl_->bus, &usec) < 0 || usec == UINT64_MAX) return -1;
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now = uint64_t(ts.tv_sec) * 1000000 + uint64_t(ts.tv_nsec) / 1000;
  return usec <= now ? 0 : int((usec - now + 999) / 1000);
}

void DbusService::process() {
  while (sd_bus_process(impl_->bus, nullptr) > 0) {
  }
}

void DbusService::emit_stats(double fps, uint64_t frames, const std::string &game, uint32_t flags) {
  sd_bus_emit_signal(impl_->bus, kObjectPath, kInterface, "StatsChanged", "dtsu", fps, frames, game.c_str(), flags);
}

void DbusService::emit_file_saved(uint64_t request_id, const std::string &kind, const std::string &path) {
  sd_bus_emit_signal(impl_->bus, kObjectPath, kInterface, "FileSaved", "tss", request_id, kind.c_str(), path.c_str());
}

void DbusService::emit_error(uint64_t request_id, const std::string &message) {
  sd_bus_emit_signal(impl_->bus, kObjectPath, kInterface, "Error", "ts", request_id, message.c_str());
}

void DbusService::notify(const std::string &summary, const std::string &body, const std::string &icon) {
  sd_bus_message *m = nullptr;
  if (sd_bus_message_new_method_call(impl_->bus, &m, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                     "org.freedesktop.Notifications", "Notify") < 0)
    return;
  sd_bus_message_append(m, "susss", "Framux", uint32_t(0), icon.c_str(), summary.c_str(), body.c_str());
  sd_bus_message_append(m, "as", 0);
  sd_bus_message_open_container(m, 'a', "{sv}");
  sd_bus_message_close_container(m);
  sd_bus_message_append(m, "i", int32_t(4000));
  // Assíncrono e sem resposta: sem servidor de notificações, nada acontece.
  sd_bus_call_async(impl_->bus, nullptr, m, nullptr, nullptr, 0);
  sd_bus_message_unref(m);
}

}  // namespace framux::daemon
