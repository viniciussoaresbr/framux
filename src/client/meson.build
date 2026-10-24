framux_client = static_library('framux_client',
  files('runtime.cpp', 'x11_input.cpp'),
  include_directories : [inc, config_inc],
  dependencies : [x11_headers, thread_dep],
  link_with : framux_common,
  pic : true,
  gnu_symbol_visibility : 'hidden')

framux_client_dep = declare_dependency(
  link_with : [framux_client, framux_common],
  include_directories : [inc, config_inc],
  dependencies : [thread_dep, dl_dep])
