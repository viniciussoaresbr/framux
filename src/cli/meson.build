framux_cli = executable('framux',
  files('doctor.cpp', 'flatpak_cmd.cpp', 'main.cpp', 'run.cpp'),
  include_directories : include_directories('..'),
  dependencies : [framux_core_dep, systemd_dep, dl_dep],
  install : true)
