// dbus_service.h no Windows: pipe de controle (common/control_proto.h).
//
// Uma thread por conexão lê os pedidos e os entrega à thread principal pela
// MainQueue (os handlers mexem no estado do daemon); a resposta volta pela
// mesma conexão. Conexões que pediram Subscribe recebem os eventos.
#include <windows.h>

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "common/control_proto.h"
#include "common/ipc_socket.h"
#include "common/log.h"
#include "daemon/dbus_service.h"
#include "daemon/worker.h"

namespace framux::daemon {

struct DbusService::Impl {
  DbusHandlers h;
  MainQueue *queue = nullptr;
  ipc::PipeListener listener;
  HANDLE stop = nullptr;
  std::mutex mu;
  std::map<int, std::thread> readers;  // mu
  std::vector<int> subscribers;        // mu
  std::atomic<bool> stopping{false};

  void reader(int sock);
  std::string handle(const std::vector<std::string> &req);
  void broadcast(const std::vector<std::string> &fields);
};

namespace {

std::string reply(const ActionResult &r, const std::string &ok_text) {
  if (!r.ok) return ctl::join({"ERR", r.error_name, r.message});
  return ctl::join({"OK", ok_text});
}

uint32_t to_u32(const std::string &s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 10)); }

}  // namespace

// Roda na thread principal.
std::string DbusService::Impl::handle(const std::vector<std::string> &req) {
  const std::string &m = req[0];
  auto arg = [&](size_t i) { return i < req.size() ? req[i] : std::string(); };
  if (m == "Status") return ctl::join({"OK", h.status(false)});
  if (m == "StatusJson") return ctl::join({"OK", h.status(true)});
  if (m == "Benchmark") {
    const ActionResult r = h.benchmark(arg(1), to_u32(arg(2)));
    return reply(r, r.message);
  }
  if (m == "Screenshot") {
    const ActionResult r = h.screenshot(arg(1) == "1");
    return reply(r, std::to_string(r.request_id));
  }
  if (m == "Overlay") {
    const ActionResult r = h.overlay(arg(1));
    return reply(r, r.message);
  }
  if (m == "Record" || m == "Loop") {
    const ActionResult r = h.video(m == "Record" ? "record" : "loop", arg(1));
    return reply(r, r.message);
  }
  if (m == "Reload") {
    h.reload();
    return "OK";
  }
  if (m == "Quit") {
    if (h.quit) h.quit();
    return "OK";
  }
  return ctl::join({"ERR", "io.github.framux.Error.UnknownMethod", "método desconhecido: " + m});
}

void DbusService::Impl::reader(int sock) {
  SetThreadDescription(GetCurrentThread(), L"framuxd-ctl");
  bool subscribed = false;
  while (!stopping) {
    ipc::Message m;
    const auto r = ipc::recv_msg_wait(sock, m, stop, INFINITE);
    if (r == ipc::RecvResult::WouldBlock) continue;
    if (r != ipc::RecvResult::Ok || m.type != ctl::kCtlRequest) break;
    const std::vector<std::string> req = ctl::split(std::string(m.payload.begin(), m.payload.end()));
    std::string out;
    if (req[0] == "Subscribe") {
      if (!subscribed) {
        std::lock_guard lk(mu);
        subscribers.push_back(sock);
        subscribed = true;
      }
      out = "OK";
    } else {
      auto done = std::make_shared<std::promise<std::string>>();
      std::future<std::string> fut = done->get_future();
      queue->post([this, req, done] { done->set_value(handle(req)); });
      if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready) break;
      out = fut.get();
    }
    if (!ipc::send_msg(sock, ctl::kCtlReply, out.data(), out.size())) break;
  }
  std::lock_guard lk(mu);
  std::erase(subscribers, sock);
  ipc::pipe_close(sock);
  // A thread não pode se juntar a si mesma: quem a encerra é o destrutor.
}

void DbusService::Impl::broadcast(const std::vector<std::string> &fields) {
  const std::string msg = ctl::join(fields);
  std::lock_guard lk(mu);
  for (int sock : subscribers) ipc::send_msg(sock, ctl::kCtlEvent, msg.data(), msg.size(), nullptr, 0, nullptr, 0,
                                             MSG_DONTWAIT);
}

DbusService::~DbusService() {
  if (!impl_) return;
  impl_->stopping = true;
  SetEvent(impl_->stop);
  impl_->listener.stop();
  std::map<int, std::thread> readers;
  {
    std::lock_guard lk(impl_->mu);
    readers.swap(impl_->readers);
  }
  for (auto &[sock, t] : readers)
    if (t.joinable()) t.join();
  CloseHandle(impl_->stop);
}

std::unique_ptr<DbusService> DbusService::open(DbusHandlers h, bool *already_running, MainQueue &queue) {
  *already_running = false;
  std::unique_ptr<DbusService> s(new DbusService());
  s->impl_ = std::make_unique<Impl>();
  Impl *d = s->impl_.get();
  d->h = std::move(h);
  d->queue = &queue;
  d->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::string err;
  const bool ok = d->listener.start(
      ctl::control_pipe_name(),
      [d](int sock) {
        std::lock_guard lk(d->mu);
        // Threads de conexões já encerradas: solta antes de criar outra.
        for (auto it = d->readers.begin(); it != d->readers.end();) {
          if (ipc::pipe_peer_pid(it->first) == 0 && it->second.joinable()) {
            it->second.detach();
            it = d->readers.erase(it);
          } else {
            ++it;
          }
        }
        d->readers[sock] = std::thread([d, sock] { d->reader(sock); });
      },
      &err);
  if (!ok) {
    *already_running = err.find("outro framuxd") != std::string::npos;
    if (!*already_running) FRAMUX_LOG("controle: %s", err.c_str());
    CloseHandle(d->stop);
    s->impl_.reset();
    return nullptr;
  }
  return s;
}

void DbusService::emit_stats(double fps, uint64_t frames, const std::string &game, uint32_t flags) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", fps);
  impl_->broadcast({"StatsChanged", buf, std::to_string(frames), game, std::to_string(flags)});
}

void DbusService::emit_file_saved(uint64_t request_id, const std::string &kind, const std::string &path) {
  impl_->broadcast({"FileSaved", std::to_string(request_id), kind, path});
}

void DbusService::emit_error(uint64_t request_id, const std::string &message) {
  impl_->broadcast({"Error", std::to_string(request_id), message});
}

// Sem servidor de notificações no Windows: a GUI (na bandeja) mostra o aviso.
void DbusService::notify(const std::string &summary, const std::string &body, const std::string &icon) {
  impl_->broadcast({"Notify", summary, body, icon});
}

}  // namespace framux::daemon
