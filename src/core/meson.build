framux_core = static_library('framux_core',
  files('arbiter.cpp', 'bench_files.cpp', 'bench_stats.cpp', 'image.cpp', 'naming.cpp'),
  include_directories : [inc, config_inc],
  dependencies : [png_dep, jpeg_dep, vulkan_headers],
  link_with : framux_common)

framux_core_dep = declare_dependency(
  link_with : [framux_core, framux_common],
  include_directories : [inc, config_inc],
  dependencies : [png_dep, jpeg_dep, thread_dep])
