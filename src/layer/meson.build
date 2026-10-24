framux_vk = shared_library('framux_vk',
  files('vk_layer.cpp'),
  dependencies : [framux_client_dep, vulkan_headers, x11_headers],
  gnu_symbol_visibility : 'hidden',
  link_args : inject_link_args,
  install : true,
  install_dir : framux_libdir)

# Pelo tamanho do ponteiro: no `rpmbuild --target i686` (gcc -m32 num host
# x86_64) o meson ainda informa cpu_family x86_64.
layer_arch = host_machine.cpu_family()
if layer_arch in ['x86', 'x86_64']
  layer_arch = is_64bit ? 'x86_64' : 'i686'
endif

# Um nome por arquitetura: com o mesmo nome nos dois manifestos, o loader
# encontra o de 32 bits primeiro num processo 64 bits, rejeita ("wrong
# bit-type") e descarta o outro por ser duplicado.

# Manifesto para uso sem instalar (VK_ADD_IMPLICIT_LAYER_PATH, `meson devenv`).
configure_file(input : 'framux_layer.json.in',
  output : 'framux_dev_@0@.json'.format(layer_arch),
  configuration : {'LIBRARY_PATH' : './libframux_vk.so', 'ARCH' : layer_arch})

# Manifesto instalado em /usr/share/vulkan/implicit_layer.d.
install_manifest = configure_file(input : 'framux_layer.json.in',
  output : 'framux_@0@.json.install'.format(layer_arch),
  configuration : {'LIBRARY_PATH' : prefix / framux_libdir / 'libframux_vk.so', 'ARCH' : layer_arch})
install_data(install_manifest,
  rename : 'framux_@0@.json'.format(layer_arch),
  install_dir : datadir / 'vulkan' / 'implicit_layer.d')
