common_src = files(
  'config.cpp',
  'fps_counter.cpp',
  'hotkey.cpp',
  'log.cpp',
  'overlay.cpp',
)
common_deps = [toml_dep, x11_headers]
if is_windows
  common_src += files('control_client_win.cpp', 'flatpak_win.cpp', 'identity_win.cpp', 'ipc_socket_win.cpp', 'paths_win.cpp',
                     'shm_win.cpp', 'win_setup_win.cpp')
  common_deps += [win_libs['shell32'], win_libs['ole32'], win_libs['advapi32'], win_libs['uuid'], win_libs['user32']]
else
  common_src += files('flatpak.cpp', 'identity.cpp', 'ipc_socket.cpp', 'paths.cpp')
endif

# Estática e PIC: entra na camada, no shim, no daemon e na CLI.
framux_common = static_library('framux_common', common_src,
  include_directories : [inc, config_inc],
  dependencies : common_deps,
  pic : true,
  gnu_symbol_visibility : 'hidden')

framux_common_dep = declare_dependency(
  link_with : framux_common,
  include_directories : [inc, config_inc],
  dependencies : is_windows ? common_deps : [])
