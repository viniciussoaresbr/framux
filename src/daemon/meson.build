framuxd = executable('framuxd',
  files('daemon.cpp', 'dbus_service.cpp', 'main.cpp', 'worker.cpp', 'x11_hotkeys.cpp'),
  dependencies : [framux_core_dep, systemd_dep, x11_dep, thread_dep],
  install : true)
