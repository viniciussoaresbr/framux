# framuxd no Windows: loop com named pipes (daemon_win.cpp), canal de
# controle no lugar do D-Bus e áudio por WASAPI.
d = '../'

framux_audio = static_library('framux_audio',
  files(d + 'game_audio_win.cpp', d + 'mic_audio_win.cpp', d + 'wasapi_capture.cpp'),
  include_directories : [inc, config_inc],
  dependencies : [framux_common_dep, win_libs['ole32'], win_libs['mmdevapi'], win_libs['avrt'], win_libs['ksuser'],
                  win_libs['uuid']])
framux_audio_dep = declare_dependency(link_with : framux_audio,
  dependencies : [win_libs['ole32'], win_libs['mmdevapi'], win_libs['avrt'], win_libs['ksuser'], win_libs['uuid']])

# Gravação com FFmpeg: ligada se ele for encontrado (-Drecording=disabled
# compila sem, com o recorder_stub.cpp).
video_deps = []
foreach name : ['libavcodec', 'libavformat', 'libavutil', 'libswscale']
  video_deps += dependency(name, required : get_option('recording'))
endforeach
have_ffmpeg = true
foreach dep : video_deps
  have_ffmpeg = have_ffmpeg and dep.found()
endforeach
recording = have_ffmpeg and not get_option('recording').disabled()

if have_ffmpeg
  framux_video = static_library('framux_video', files(d + 'recorder.cpp'),
    dependencies : [framux_core_dep, thread_dep, video_deps, framux_audio_dep],
    build_by_default : recording)
endif

daemon_src = files(d + 'daemon.cpp', d + 'daemon_ext_win.cpp', d + 'daemon_win.cpp', d + 'dbus_service_win.cpp',
                   d + 'etw_presents_win.cpp', d + 'main.cpp', d + 'overlay_window_win.cpp', d + 'window_capture_win.cpp',
                   d + 'worker.cpp', '../../client/win_input.cpp')
# ETW (advapi32), captura de janela (d3d11, dxgi, WinRT) e módulos do jogo (psapi).
daemon_deps = [framux_core_dep, framux_audio_dep, thread_dep, win_libs['user32'], win_libs['advapi32'],
               win_libs['gdi32'], win_libs['psapi'], win_libs['dwmapi'], cpp.find_library('d3d11'), cpp.find_library('dxgi'),
               cpp.find_library('runtimeobject')]
if recording
  daemon_deps += declare_dependency(link_with : framux_video, dependencies : video_deps)
else
  daemon_src += files(d + 'recorder_stub.cpp')
endif

framuxd = executable('framuxd', daemon_src, windows_res,
  include_directories : [inc, config_inc],
  dependencies : daemon_deps,
  win_subsystem : 'windows',
  install : true)
