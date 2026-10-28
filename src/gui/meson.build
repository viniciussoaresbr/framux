qt6_dep = dependency('qt6', modules : ['Core', 'Gui', 'Widgets', 'DBus', 'Network'],
  required : get_option('gui'))
if not qt6_dep.found()
  subdir_done()
endif
# Lista de microfones e medidor de nível na aba Gravações.
pipewire_dep = dependency('libpipewire-0.3')

# Ícone e nome embutidos no executável (assets/icons/). Vão como fonte em
# framux_gui_dep, não na biblioteca estática: lá o linker descartaria o objeto
# que só registra os recursos.
framux_res = import('qt6').compile_resources(sources : files('framux.qrc'))

# Sem Q_OBJECT: os widgets avisam por callback, então não há moc.
framux_gui_lib = static_library('framux_gui',
  files('audio_devices.cpp', 'main_window.cpp', 'settings.cpp', 'theme.cpp', 'widgets.cpp'),
  include_directories : include_directories('..'),
  dependencies : [framux_common_dep, qt6_dep, x11_headers, pipewire_dep])
framux_gui_dep = declare_dependency(link_with : framux_gui_lib,
  sources : framux_res,
  include_directories : include_directories('..'),
  dependencies : [framux_common_dep, qt6_dep, pipewire_dep])

framux_gui = executable('framux-gui', files('main.cpp'),
  dependencies : framux_gui_dep,
  install : true)
