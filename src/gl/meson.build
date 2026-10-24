framux_gl = shared_library('framux_gl',
  files('gl_shim.cpp'),
  dependencies : [framux_client_dep, gl_headers, egl_headers, x11_headers, vulkan_headers],
  gnu_symbol_visibility : 'hidden',
  link_args : inject_link_args,
  install : true,
  install_dir : framux_libdir)
