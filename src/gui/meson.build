# No Windows não há D-Bus: a janela fala com o framuxd pelo canal de controle.
qt6_dep = dependency('qt6', modules : is_windows ? ['Core', 'Gui', 'Widgets', 'Network'] : ['Core', 'Gui', 'Widgets', 'DBus', 'Network'],
  required : get_option('gui'))
if not qt6_dep.found()
  subdir_done()
endif
# Lista de microfones e medidor de nível na aba Gravações.
if is_windows
  pipewire_dep = [win_libs['ole32'], win_libs['uuid']]  # WASAPI
else
  pipewire_dep = dependency('libpipewire-0.3')
endif

# Ícone e nome embutidos no executável (assets/icons/). Vão como fonte em
# framux_gui_dep, não na biblioteca estática: lá o linker descartaria o objeto
# que só registra os recursos.
framux_res = import('qt6').compile_resources(sources : files('framux.qrc'))

# Sem Q_OBJECT: os widgets avisam por callback, então não há moc.
framux_gui_lib = static_library('framux_gui',
  files(is_windows ? 'audio_devices_win.cpp' : 'audio_devices.cpp', 'main_window.cpp', 'settings.cpp', 'theme.cpp',
        'widgets.cpp'),
  include_directories : include_directories('..'),
  dependencies : [framux_common_dep, qt6_dep, x11_headers, pipewire_dep])
framux_gui_dep = declare_dependency(link_with : framux_gui_lib,
  sources : framux_res,
  include_directories : include_directories('..'),
  dependencies : [framux_common_dep, qt6_dep, pipewire_dep])

framux_gui = executable('framux-gui', files('main.cpp'), windows_res,
  dependencies : framux_gui_dep,
  win_subsystem : is_windows ? 'windows' : 'console',
  install : true)
