common_src = files(
  'config.cpp',
  'flatpak.cpp',
  'fps_counter.cpp',
  'hotkey.cpp',
  'identity.cpp',
  'ipc_socket.cpp',
  'log.cpp',
  'overlay.cpp',
  'paths.cpp',
)

# Estática e PIC: entra na camada, no shim, no daemon e na CLI.
framux_common = static_library('framux_common', common_src,
  include_directories : [inc, config_inc],
  dependencies : [toml_dep, x11_headers],
  pic : true,
  gnu_symbol_visibility : 'hidden')

framux_common_dep = declare_dependency(
  link_with : framux_common,
  include_directories : [inc, config_inc])
