if is_windows
  framux_cli = executable('framux',
    files('doctor_win.cpp', 'main_win.cpp', 'run_win.cpp'), windows_res,
    include_directories : include_directories('..'),
    dependencies : [framux_core_dep, win_libs['shell32']],
    install : true)
  subdir_done()
endif

framux_cli = executable('framux',
  files('doctor.cpp', 'flatpak_cmd.cpp', 'main.cpp', 'run.cpp'),
  include_directories : include_directories('..'),
  dependencies : [framux_core_dep, systemd_dep, dl_dep],
  install : true)
