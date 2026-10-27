project('framux', ['c', 'cpp'],
  version : '0.4.3',
  license : 'MIT',
  meson_version : '>= 1.1.0',
  default_options : [
    'cpp_std=c++20',
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
    'b_ndebug=if-release',
  ])

cpp = meson.get_compiler('cpp')
is_64bit = cpp.sizeof('void*') == 8
layer_only = get_option('layer_only')

add_project_arguments(
  cpp.get_supported_arguments(['-Wno-missing-field-initializers', '-Wno-unused-parameter']),
  '-D_GNU_SOURCE',
  language : 'cpp')

prefix = get_option('prefix')
libdir = get_option('libdir')
datadir = get_option('datadir')

# Biblioteca da camada/shim: /usr/lib64/framux (x86_64) ou /usr/lib/framux (i686).
framux_libdir = libdir / 'framux'

# Caminho do shim para o LD_PRELOAD do wrapper. O token $LIB do ld.so resolve
# para lib64 ou lib conforme a classe ELF do processo, então um único valor
# atende jogos 64 e 32 bits.
lib_token_dir = prefix / '$LIB' / 'framux'
if libdir.contains('/')
  # libdir absoluto e fora do padrão: sem token, só a arquitetura nativa.
  lib_token_dir = libdir / 'framux'
endif

conf = configuration_data()
conf.set_quoted('FRAMUX_VERSION', meson.project_version())
conf.set_quoted('FRAMUX_PRELOAD_DIR', lib_token_dir)
conf.set_quoted('FRAMUX_LIBEXEC_DAEMON', prefix / get_option('bindir') / 'framuxd')
conf.set_quoted('FRAMUX_BINDIR', prefix / get_option('bindir'))
# Camada e shim de cada arquitetura, copiados para o Steam Flatpak.
conf.set_quoted('FRAMUX_LIBDIR64', prefix / libdir / 'framux')
conf.set_quoted('FRAMUX_LIBDIR32', prefix / 'lib' / 'framux')
conf.set_quoted('FRAMUX_DATADIR', prefix / datadir)
conf.set_quoted('FRAMUX_SYSCONFDIR', prefix / get_option('sysconfdir'))
configure_file(output : 'framux_build_config.h', configuration : conf)
config_inc = include_directories('.')

# Camada e shim rodam dentro do jogo, às vezes com outra libstdc++ (runtime da
# Steam, Flatpak): libstdc++/libgcc entram estáticas e sem exportar símbolos.
inject_link_args = cpp.get_supported_link_arguments(['-Wl,--no-undefined', '-Wl,-z,nodelete'])
static_cxx = ['-static-libstdc++', '-static-libgcc', '-Wl,--exclude-libs,ALL']
if cpp.has_multi_link_arguments(static_cxx)
  inject_link_args += static_cxx
else
  warning('libstdc++-static ausente: camada e shim dependerão da libstdc++ do jogo')
endif

thread_dep = dependency('threads')
dl_dep = cpp.find_library('dl', required : false)
# Para a camada e o shim bastam os cabeçalhos (libX11/libGL/libEGL são
# carregadas com dlopen ou já estão no processo do jogo). No build i686 os .pc
# de 64 bits não servem, então cai para a checagem direta do cabeçalho.
header_deps = {}
foreach name, header : {'vulkan' : 'vulkan/vulkan.h', 'x11' : 'X11/Xlib.h', 'gl' : 'GL/glx.h', 'egl' : 'EGL/egl.h',
                        'xcb' : 'xcb/xinput.h'}
  d = dependency(name, required : false)
  if d.found()
    header_deps += {name : d.partial_dependency(compile_args : true, includes : true)}
  elif cpp.has_header(header)
    header_deps += {name : declare_dependency()}
  else
    error('cabeçalho @0@ não encontrado (pacote de desenvolvimento de @1@)'.format(header, name))
  endif
endforeach
if not cpp.has_header('vulkan/utility/vk_dispatch_table.h')
  error('vulkan-utility-libraries-devel não encontrado')
endif
# toml++ em modo header-only (TOML_HEADER_ONLY=1 em config.cpp): a camada não
# deve depender de libtomlplusplus.so no processo do jogo.
if not cpp.has_header('toml++/toml.hpp')
  error('toml++ não encontrado (dnf install tomlplusplus-devel)')
endif
toml_dep = declare_dependency()

vulkan_headers = header_deps['vulkan']
x11_headers = [header_deps['x11'], header_deps['xcb']]
gl_headers = header_deps['gl']
egl_headers = header_deps['egl']

inc = include_directories('include', 'src')

subdir('src/common')
subdir('src/client')
subdir('src/layer')
subdir('src/gl')

if not layer_only
  png_dep = dependency('libpng', required : true)
  jpeg_dep = dependency('libjpeg', required : true)
  systemd_dep = dependency('libsystemd', required : true)
  x11_dep = dependency('x11', required : true)

  subdir('src/core')
  subdir('src/daemon')
  subdir('src/cli')
  subdir('src/gui')
  subdir('data')
  install_headers('include/framux/stats.h', subdir : 'framux')
endif

if get_option('tests')
  subdir('tests')
endif

# `meson devenv -C build` permite rodar `framux run` sem instalar.
devenv = environment()
devenv.set('FRAMUX_GL_PRELOAD', meson.current_build_dir() / 'src/gl/libframux_gl.so')
devenv.prepend('VK_ADD_IMPLICIT_LAYER_PATH', meson.current_build_dir() / 'src/layer')
devenv.set('FRAMUX_DAEMON', meson.current_build_dir() / 'src/daemon/framuxd')
devenv.prepend('PATH', meson.current_build_dir() / 'src/cli', meson.current_build_dir() / 'src/daemon')
meson.add_devenv(devenv)
