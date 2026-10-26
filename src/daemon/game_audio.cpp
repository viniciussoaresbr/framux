#include "daemon/game_audio.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <time.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include "common/log.h"

namespace framux::daemon {

namespace {

constexpr uint32_t kRate = 48000;

uint64_t monotonic_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

const char *get(const spa_dict *d, const char *key) { return d ? spa_dict_lookup(d, key) : nullptr; }

// Canal de uma porta: audio.channel ou, na falta, o sufixo de port.name
// ("output_FL" → "FL").
std::string port_channel(const spa_dict *props) {
  if (const char *c = get(props, PW_KEY_AUDIO_CHANNEL)) return c;
  if (const char *n = get(props, PW_KEY_PORT_NAME)) {
    const char *u = std::strrchr(n, '_');
    return u ? u + 1 : n;
  }
  return {};
}

}  // namespace

struct GameAudio::Impl {
  struct Node {
    Impl *self = nullptr;
    uint32_t id = 0;
    pw_proxy *proxy = nullptr;
    spa_hook listener{};
    int app_pid = 0;                   // application.process.id do nó (pipewire-pulse)
    uint32_t client = SPA_ID_INVALID;  // client.id (streams nativos levam o PID no client)
    bool matched = false;
  };
  struct Client {
    int app_pid = 0;
    int sec_pid = 0;  // pipewire.sec.pid: o PID visto pelo servidor
  };
  struct Port {
    uint32_t node;
    std::string channel;
    bool output;
  };

  int pid = 0, peer_pid = 0;
  Sink sink;
  pw_thread_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  pw_stream *stream = nullptr;
  spa_hook registry_listener{}, stream_listener{};
  uint32_t our_node = SPA_ID_INVALID;
  std::map<uint32_t, std::unique_ptr<Node>> nodes;  // saídas de áudio de apps (vinculadas)
  std::map<uint32_t, Client> clients;
  std::map<uint32_t, Port> ports;
  std::map<std::pair<uint32_t, uint32_t>, pw_proxy *> links;  // (porta do jogo, nossa porta)
  bool logged_link = false;

  static int int_prop(const spa_dict *props, const char *key) {
    const char *v = get(props, key);
    return v ? std::atoi(v) : 0;
  }

  // Do jogo: pelo PID que o nó anuncia (pipewire-pulse: Wine/Proton, SDL com
  // PulseAudio) ou pelo do client dele (streams nativos do PipeWire).
  bool matches(const Node &n) const {
    auto is_game = [this](int v) { return v > 0 && (v == pid || v == peer_pid); };
    if (is_game(n.app_pid)) return true;
    auto c = clients.find(n.client);
    return c != clients.end() && (is_game(c->second.app_pid) || (peer_pid > 0 && c->second.sec_pid == peer_pid));
  }

  void update_node(Node &n, const spa_dict *props) {
    if (const int v = int_prop(props, PW_KEY_APP_PROCESS_ID)) n.app_pid = v;
    if (const char *c = get(props, PW_KEY_CLIENT_ID)) n.client = uint32_t(std::strtoul(c, nullptr, 10));
    const bool m = matches(n);
    FRAMUX_DEBUG("áudio: nó %u (pid %d, client %u)%s", n.id, n.app_pid, n.client, m ? " é do jogo" : "");
    if (m == n.matched) return;
    n.matched = m;
    if (m) try_link();
  }

  // Liga as saídas FL/FR (ou MONO nas duas) do jogo às nossas entradas.
  void try_link() {
    if (our_node == SPA_ID_INVALID) return;
    std::map<std::string, uint32_t> ours;
    for (const auto &[id, p] : ports)
      if (p.node == our_node && !p.output) ours[p.channel] = id;
    if (ours.empty()) return;
    for (const auto &[id, p] : ports) {
      if (!p.output) continue;
      auto n = nodes.find(p.node);
      if (n == nodes.end() || !n->second->matched) continue;
      std::vector<uint32_t> targets;
      if (p.channel == "MONO") {
        for (const char *c : {"FL", "FR"})
          if (ours.count(c)) targets.push_back(ours[c]);
      } else if (ours.count(p.channel)) {
        targets.push_back(ours[p.channel]);
      }
      for (uint32_t in : targets) {
        if (links.count({id, in})) continue;
        pw_properties *lp = pw_properties_new(PW_KEY_OBJECT_LINGER, "false", nullptr);
        pw_properties_setf(lp, PW_KEY_LINK_OUTPUT_NODE, "%u", p.node);
        pw_properties_setf(lp, PW_KEY_LINK_OUTPUT_PORT, "%u", id);
        pw_properties_setf(lp, PW_KEY_LINK_INPUT_NODE, "%u", our_node);
        pw_properties_setf(lp, PW_KEY_LINK_INPUT_PORT, "%u", in);
        auto *proxy = static_cast<pw_proxy *>(
            pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &lp->dict, 0));
        pw_properties_free(lp);
        if (proxy) links[{id, in}] = proxy;
        FRAMUX_DEBUG("áudio: ligando porta %u (%s) → %u", id, p.channel.c_str(), in);
        if (!logged_link) {
          logged_link = true;
          FRAMUX_LOG("gravação: capturando o áudio do jogo (nó %u do PipeWire)", p.node);
        }
      }
    }
  }

  static void on_node_info(void *data, const pw_node_info *info) {
    auto *n = static_cast<Node *>(data);
    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) n->self->update_node(*n, info->props);
  }

  static void on_global(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props) {
    auto *self = static_cast<Impl *>(data);
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
      const char *cls = get(props, PW_KEY_MEDIA_CLASS);
      if (!cls || std::strcmp(cls, "Stream/Output/Audio") != 0) return;
      // As propriedades globais nem sempre trazem o PID: vincula e lê o info.
      auto n = std::make_unique<Node>();
      n->self = self;
      n->id = id;
      n->proxy = static_cast<pw_proxy *>(pw_registry_bind(self->registry, id, type, PW_VERSION_NODE, 0));
      if (!n->proxy) return;
      static const pw_node_events events = {PW_VERSION_NODE_EVENTS, on_node_info, nullptr};
      pw_proxy_add_object_listener(n->proxy, &n->listener, &events, n.get());
      Node &ref = *n;
      self->nodes[id] = std::move(n);
      self->update_node(ref, props);
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
      self->clients[id] = Client{int_prop(props, PW_KEY_APP_PROCESS_ID), int_prop(props, PW_KEY_SEC_PID)};
      for (auto &[nid, n] : self->nodes)
        if (n->client == id) self->update_node(*n, nullptr);
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
      const char *node = get(props, PW_KEY_NODE_ID), *dir = get(props, PW_KEY_PORT_DIRECTION);
      if (!node || !dir) return;
      const char *monitor = get(props, PW_KEY_PORT_MONITOR);
      if (monitor && std::strcmp(monitor, "true") == 0) return;
      self->ports[id] =
          Port{uint32_t(std::strtoul(node, nullptr, 10)), port_channel(props), std::strcmp(dir, "out") == 0};
      FRAMUX_DEBUG("áudio: porta %u do nó %s, %s %s", id, node, dir, self->ports[id].channel.c_str());
      self->try_link();
    }
  }

  static void on_global_remove(void *data, uint32_t id) {
    auto *self = static_cast<Impl *>(data);
    if (auto n = self->nodes.find(id); n != self->nodes.end()) {
      spa_hook_remove(&n->second->listener);
      pw_proxy_destroy(n->second->proxy);
      self->nodes.erase(n);
    }
    self->ports.erase(id);
    self->clients.erase(id);
    for (auto it = self->links.begin(); it != self->links.end();) {
      if (it->first.first == id || it->first.second == id) {
        pw_proxy_destroy(it->second);
        it = self->links.erase(it);
      } else {
        ++it;
      }
    }
  }

  static void on_state(void *data, pw_stream_state, pw_stream_state state, const char *error) {
    auto *self = static_cast<Impl *>(data);
    if (state == PW_STREAM_STATE_ERROR) FRAMUX_LOG("gravação: stream de áudio com erro: %s", error ? error : "?");
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
      const uint32_t id = pw_stream_get_node_id(self->stream);
      FRAMUX_DEBUG("áudio: stream de captura %s, nó %u", pw_stream_state_as_string(state), id);
      if (id != self->our_node) {
        self->our_node = id;
        self->try_link();
      }
    }
  }

  static void on_process(void *data) {
    auto *self = static_cast<Impl *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(self->stream);
    if (!b) return;
    const spa_data &d = b->buffer->datas[0];
    if (d.data && d.chunk) {
      const uint32_t frames = d.chunk->size / (2 * sizeof(float));
      pw_time t{};
      uint64_t when = monotonic_ns();
      if (pw_stream_get_time_n(self->stream, &t, sizeof t) == 0 && t.now > 0) {
        when = uint64_t(t.now);
        if (t.rate.denom && t.delay > 0) when -= uint64_t(t.delay) * 1'000'000'000ull * t.rate.num / t.rate.denom;
      }
      const auto *samples = reinterpret_cast<const float *>(static_cast<const uint8_t *>(d.data) + d.chunk->offset);
      if (frames) self->sink(samples, frames, when);
    }
    pw_stream_queue_buffer(self->stream, b);
  }

  void teardown() {
    if (loop) pw_thread_loop_lock(loop);
    for (auto &[k, p] : links) pw_proxy_destroy(p);
    links.clear();
    for (auto &[id, n] : nodes) {
      spa_hook_remove(&n->listener);
      pw_proxy_destroy(n->proxy);
    }
    nodes.clear();
    if (stream) pw_stream_destroy(stream);
    stream = nullptr;
    if (registry) {
      spa_hook_remove(&registry_listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    }
    registry = nullptr;
    if (core) pw_core_disconnect(core);
    core = nullptr;
    if (loop) pw_thread_loop_unlock(loop);
    if (loop) pw_thread_loop_stop(loop);
    if (context) pw_context_destroy(context);
    context = nullptr;
    if (loop) pw_thread_loop_destroy(loop);
    loop = nullptr;
  }
};

std::unique_ptr<GameAudio> GameAudio::start(int pid, int peer_pid, Sink sink, std::string *err) {
  static std::once_flag once;
  std::call_once(once, [] { pw_init(nullptr, nullptr); });

  std::unique_ptr<GameAudio> ga(new GameAudio);
  ga->impl_ = std::make_unique<Impl>();
  Impl &im = *ga->impl_;
  im.pid = pid;
  im.peer_pid = peer_pid;
  im.sink = std::move(sink);
  im.loop = pw_thread_loop_new("framux-audio", nullptr);
  if (!im.loop) {
    *err = "PipeWire indisponível";
    return nullptr;
  }
  im.context = pw_context_new(pw_thread_loop_get_loop(im.loop), nullptr, 0);
  pw_thread_loop_lock(im.loop);
  if (!im.context || !(im.core = pw_context_connect(im.context, nullptr, 0))) {
    pw_thread_loop_unlock(im.loop);
    *err = "não consegui conectar ao PipeWire";
    im.teardown();
    return nullptr;
  }

  static const pw_registry_events reg_events = {PW_VERSION_REGISTRY_EVENTS, Impl::on_global, Impl::on_global_remove};
  im.registry = pw_core_get_registry(im.core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(im.registry, &im.registry_listener, &reg_events, &im);

  // Sem autoconnect: o WirePlumber não liga o stream a microfone nenhum; só
  // as ligações feitas aqui, para as saídas do jogo.
  pw_properties *props =
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Production",
                        PW_KEY_NODE_NAME, "framux-game-audio", PW_KEY_NODE_DESCRIPTION, "Framux (áudio do jogo)",
                        PW_KEY_APP_NAME, "Framux", PW_KEY_NODE_DONT_RECONNECT, "true", nullptr);
  im.stream = pw_stream_new(im.core, "framux-game-audio", props);
  static pw_stream_events stream_events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = Impl::on_state;
    e.process = Impl::on_process;
    return e;
  }();
  pw_stream_add_listener(im.stream, &im.stream_listener, &stream_events, &im);

  uint8_t buf[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = kRate;
  info.channels = 2;
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;
  const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
  const int r = pw_stream_connect(im.stream, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
  pw_thread_loop_unlock(im.loop);
  if (r < 0 || pw_thread_loop_start(im.loop) < 0) {
    *err = "não consegui criar o stream de captura no PipeWire";
    im.teardown();
    return nullptr;
  }
  return ga;
}

GameAudio::~GameAudio() {
  if (impl_) impl_->teardown();
}

}  // namespace framux::daemon
