# Gravação: FFmpeg (ffmpeg-free-devel no Fedora) e PipeWire (pipewire-devel).
video_deps = []
foreach name : ['libavcodec', 'libavformat', 'libavutil', 'libswscale', 'libpipewire-0.3']
  video_deps += dependency(name, required : false)
  if not video_deps[-1].found()
    error('@0@ não encontrado: instale ffmpeg-free-devel e pipewire-devel'.format(name))
  endif
endforeach

# Gravação e áudio numa biblioteca à parte: também usada pelos testes.
framux_video = static_library('framux_video', files('game_audio.cpp', 'mic_audio.cpp', 'recorder.cpp'),
  dependencies : [framux_core_dep, thread_dep, video_deps])
framux_video_dep = declare_dependency(link_with : framux_video, dependencies : [framux_core_dep, video_deps])

framuxd = executable('framuxd',
  files('daemon.cpp', 'dbus_service.cpp', 'main.cpp', 'worker.cpp', 'x11_hotkeys.cpp'),
  dependencies : [framux_video_dep, systemd_dep, x11_dep, thread_dep],
  install : true)
