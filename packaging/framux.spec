# Construção:
#   x86_64: pacote completo (framux + framux-layer)
#   i686:   só a camada e o shim (framux-layer.i686, multilib)
%bcond tests 0

Name:           framux
Version:        0.4.4
Release:        1%{?dist}
Summary:        Contador de FPS, benchmark, screenshots e gravação para jogos no Linux
License:        MIT
URL:            https://github.com/viniciussoaresbr/framux
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  meson >= 1.1
BuildRequires:  gcc-c++
# Camada e shim levam a libstdc++ estática (Steam runtime, Flatpak).
BuildRequires:  libstdc++-static
BuildRequires:  vulkan-headers
BuildRequires:  vulkan-utility-libraries-devel
BuildRequires:  libX11-devel
BuildRequires:  libxcb-devel
BuildRequires:  mesa-libGL-devel
BuildRequires:  mesa-libEGL-devel
BuildRequires:  tomlplusplus-devel
%ifnarch %{ix86}
BuildRequires:  libpng-devel
BuildRequires:  libjpeg-turbo-devel
BuildRequires:  systemd-devel
BuildRequires:  systemd-rpm-macros
BuildRequires:  ffmpeg-free-devel
BuildRequires:  pipewire-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  desktop-file-utils
%if %{with tests}
BuildRequires:  catch2-devel
%endif
Requires:       %{name}-layer%{?_isa} = %{version}-%{release}
# Jogos 32 bits (ex.: Left 4 Dead 2 nativo).
Recommends:     %{name}-layer(x86-32)
%endif

%description
Framux é uma alternativa nativa ao FRAPS para Linux: contador de FPS dentro
do jogo (Vulkan, OpenGL e todo Direct3D via Proton/DXVK/VKD3D), benchmark por
hotkey com CSV moderno, screenshots e gravação do jogo com o áudio dele.
Janela de configuração com abas Geral, FPS, Gravações e Capturas, ícone na bandeja
e modo automático para jogos da Steam/Proton, inclusive o Steam Flatpak.
Sem rede e sem telemetria.

%package layer
Summary:        Camada Vulkan e shim OpenGL do Framux

%description layer
Camada Vulkan implícita (ativa só com FRAMUX=1) e shim OpenGL para LD_PRELOAD
que medem o tempo de apresentação, desenham o contador e capturam quadros.
Instale também a variante i686 para jogos 32 bits.

%prep
%autosetup

%build
%ifarch %{ix86}
%meson -Dlayer_only=true -Dtests=false
%else
%meson -Dsystemd_user_unit_dir=%{_userunitdir} -Dgui=enabled -Dtests=%{?with_tests:true}%{!?with_tests:false}
%endif
%meson_build

%install
%meson_install

%check
%ifnarch %{ix86}
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.framux.desktop
%endif
%if %{with tests}
%meson_test unit
%endif

%ifnarch %{ix86}
%post
%systemd_user_post framuxd.socket

%preun
%systemd_user_preun framuxd.socket

%files
%license LICENSE
%doc README.md
%{_bindir}/framux
%{_bindir}/framuxd
%{_bindir}/framux-gui
%{_datadir}/applications/io.github.framux.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.framux.svg
%{_datadir}/icons/hicolor/*/apps/io.github.framux.png
%{_userunitdir}/framuxd.service
%{_userunitdir}/framuxd.socket
%{_includedir}/framux/
%{_docdir}/framux/config.toml.example
%endif

%files layer
%license LICENSE
%dir %{_libdir}/framux
%{_libdir}/framux/libframux_vk.so
%{_libdir}/framux/libframux_gl.so
%{_datadir}/vulkan/implicit_layer.d/framux_*.json

%changelog
* Tue Oct 27 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.4.4-1
- Gravação: com codificador no processador (SVT-AV1, x264, Ut Video), as
  threads de conversão e codificação rodam com nice 10, para o jogo não
  travar; o Ut Video usa só metade dos núcleos
- Aba Gravações: a lista de codificadores mostra só os detectados e avisa,
  em uma linha, quando o escolhido usa o processador

* Mon Oct 26 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.4.3-1
- "Parar o Framux" também apaga o contador nos jogos: com o modo automático
  a camada seguia desenhando sozinha; agora fica quieta até o Framux ser
  aberto de novo (inclusive em jogos já abertos)

* Sun Oct 25 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.4.2-1
- Gravação: linha "medição da gravação" no log ao parar, com o FPS do jogo
  antes e durante, o custo da cópia na thread do jogo e a CPU do daemon
- Qualidade constante com teto de taxa por resolução e FPS (H.264 alto em
  2560x1080 a 60 fps: até 20 Mbit/s, ~150 MB/min); estimativa de tamanho na
  aba Gravações
- Automático prefere o codificador da placa (AV1, depois H.264) ao AV1 pelo
  processador
- Vulkan: cor convertida para NV12 na GPU (compute shader), já na metade do
  tamanho se pedido; 60% menos dados copiados por quadro e nenhuma conversão
  no processador

* Sun Oct 25 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.4.1-1
- Gravação: conversão de cor em paralelo e numa thread separada do encoder,
  com fila de quadros; quase nenhum quadro descartado em 2560x1080 a 60 fps
- Automático passa a preferir AV1 (placa de vídeo, depois SVT-AV1): o Fedora
  sem RPM Fusion decodifica H.264 devagar demais para alta resolução
- HEVC removido das opções (sem decodificador no Fedora); configs antigas
  voltam ao automático
- Sem perda passa de FFV1 para Ut Video: ~6x mais rápido, cabe em tempo
  real em 2560x1080 a 60 fps

* Sun Oct 25 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.4.0-1
- Gravação do jogo (F9 inicia/para, `framux rec`): Vulkan e OpenGL, encoder
  por hardware (NVENC/VA-API) com fallback para SVT-AV1/FFV1, taxa constante,
  tamanho cheio ou metade, MKV com conversão opcional para MP4
- Áudio só do processo do jogo pelo PipeWire, em trilha Opus
- Nova aba Gravações na janela; contador vermelho e selo vermelho na bandeja
  enquanto grava
- Protocolo IPC 2: camada e daemon precisam ser atualizados juntos

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.3.8-1
- Abas em português: Geral, FPS e Capturas (seção Capturas de Tela)

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.3.7-1
- Atalho do overlay só mostra/oculta, no canto escolhido na janela; novo
  atalho "Alternar canto" (padrão Shift+F12) e `framux overlay toggle`
- Mais espaço entre o FPS e o frame time no overlay
- Caixa de confirmação com título "Framux"

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.3.6-1
- Janela sempre no tema escuro; sem a seção Aparência (notificações sempre
  ligadas); caixa de "Parar o Framux" com Sim/Não nas cores do app
- Aba FPS: dica do canto em uma linha, sem a frase sobre o atalho

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.3.5-1
- Janela: abas com ícone e destaque em pílula nas cores da logo, só o nome
  "Framux" no topo, páginas com rolagem (avisos não comprimem mais os itens)
- Cor padrão do contador: #C6F432

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.3.4-1
- Removida a cópia no formato do FRAPS (opção da aba FPS, `fraps_compat` e
  `framux export-fraps`); a chave antiga é aceita sem efeito

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.3.3-1
- Novo ícone (framux-icons/) no menu, na janela e na bandeja; nome "Framux"
  à esquerda das abas

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.3.2-1
- Shim OpenGL: busca do RTLD_NEXT sem dlopen, sem recursão com o overlay da
  Steam (que também intercepta dlopen/dlsym)
- framux run e framux-run põem o shim no fim do LD_PRELOAD, depois do
  overlay da Steam

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.3.1-1
- framux-run do Steam Flatpak passa o shim de cada arquitetura por caminho
  explícito, sem depender de links do $LIB

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.3.0-1
- Integração com o Steam Flatpak (janela, `framux flatpak`, `framux doctor`)
- Camada e shim com libstdc++ estática, carregáveis no runtime do Flatpak e da
  Steam

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.2.0-1
- Janela de configuração (Qt 6) com abas General, FPS e Screenshots, bandeja,
  tema claro/escuro e modo automático para jogos da Steam/Proton/Wine
- Camada Vulkan com nome por arquitetura (64 e 32 bits instaladas juntas)

* Sat Oct 24 2026 Vinicius Soares <viniciussoaresmbr@gmail.com> - 0.1.0-1
- Primeira versão: camada Vulkan, shim OpenGL, daemon, hotkeys X11/in-game,
  benchmark (nativo + FRAPS), screenshots e CLI
