// Microfones do PipeWire para a aba Gravações: as fontes de áudio (entram e
// saem ao vivo) e o pico do microfone escolhido, para o medidor de nível.
// Thread própria do PipeWire; a janela consulta por polling. Sem PipeWire,
// a lista fica vazia e o nível em zero.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace framux::gui {

class AudioDevices {
 public:
  struct Source {
    std::string name;         // node.name (o valor de [audio] microphone_device)
    std::string description;  // node.description, para o combo
  };

  AudioDevices();
  ~AudioDevices();

  bool available() const;
  // Incrementa a cada fonte que entra ou sai.
  uint64_t revision() const;
  std::vector<Source> sources() const;

  // Captura `device` ("default" = padrão do sistema) para o medidor; vazio
  // para. Enquanto liga, o sistema mostra o microfone como em uso.
  void monitor(const std::string &device);
  // Pico (0..1) desde a chamada anterior.
  float take_peak();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace framux::gui
