project('framux', ['c', 'cpp'],
  version : '0.4.7',
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
# Windows (MSYS2/MinGW-w64): mesma arquitetura, com named pipes, WASAPI,
# camada Vulkan registrada no registro e framux_hook.dll para D3D/OpenGL.
is_windows = host_machine.system() == 'windows'

add_project_arguments(
  cpp.get_supported_arguments(['-Wno-missing-field-initializers', '-Wno-unused-parameter']),
  '-D_GNU_SOURCE',
  language : 'cpp')
if is_windows
  # Windows 10 1903+ (SetThreadDescription, página de código UTF-8, loopback
  # de processo no WASAPI a partir do 2004); localtime_r/gmtime_r do MinGW.
  add_project_arguments('-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A000008',
    '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_POSIX_THREAD_SAFE_FUNCTIONS=200112L',
    # printf do MinGW (C99: %zu, %llu) em vez do verificador de formato da msvcrt.
    '-D__USE_MINGW_ANSI_STDIO=1',
    language : ['c', 'cpp'])
endif

prefix = get_option('prefix')
libdir = get_option('libdir')
datadir = get_option('datadir')

# Biblioteca da camada/shim: /usr/lib64/framux (x86_64) ou /usr/lib/framux (i686).
framux_libdir = libdir / 'framux'

# Debian/Ubuntu (multiarch): libdir = lib/<tripla>, como o $LIB do ld.so.
multiarch = libdir.startswith('lib/') and libdir.endswith('-linux-gnu')

# Caminho do shim para o LD_PRELOAD do wrapper. O token $LIB do ld.so resolve
# para lib64 ou lib (Fedora), ou lib/x86_64-linux-gnu ou lib/i386-linux-gnu
# (Debian/Ubuntu), conforme a classe ELF do processo, então um único valor
# atende jogos 64 e 32 bits.
lib_token_dir = prefix / '$LIB' / 'framux'
if libdir.contains('/') and not multiarch
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
conf.set_quoted('FRAMUX_LIBDIR32', prefix / (multiarch ? 'lib/i386-linux-gnu' : 'lib') / 'framux')
# Dicas de instalação com apt em vez de dnf.
conf.set10('FRAMUX_DEBIAN', multiarch)
conf.set_quoted('FRAMUX_DATADIR', prefix / datadir)
conf.set_quoted('FRAMUX_SYSCONFDIR', prefix / get_option('sysconfdir'))
configure_file(output : 'framux_build_config.h', configuration : conf)
config_inc = include_directories('.')

# Camada e shim rodam dentro do jogo, às vezes com outra libstdc++ (runtime da
# Steam, Flatpak): libstdc++/libgcc entram estáticas e sem exportar símbolos.
if is_windows
  # No Windows tudo estático (libstdc++, libgcc, winpthread): as DLLs que
  # entram no jogo só dependem de DLLs do sistema.
  inject_link_args = ['-static', '-static-libgcc', '-static-libstdc++', '-Wl,--exclude-all-symbols']
else
  inject_link_args = cpp.get_supported_link_arguments(['-Wl,--no-undefined', '-Wl,-z,nodelete'])
  static_cxx = ['-static-libstdc++', '-static-libgcc', '-Wl,--exclude-libs,ALL']
  if cpp.has_multi_link_arguments(static_cxx)
    inject_link_args += static_cxx
  else
    warning('libstdc++-static ausente: camada e shim dependerão da libstdc++ do jogo')
  endif
endif

thread_dep = dependency('threads')
dl_dep = cpp.find_library('dl', required : false)
# Para a camada e o shim bastam os cabeçalhos (libX11/libGL/libEGL são
# carregadas com dlopen ou já estão no processo do jogo). No build i686 os .pc
# de 64 bits não servem, então cai para a checagem direta do cabeçalho.
header_deps = {}
header_list = {'vulkan' : 'vulkan/vulkan.h', 'x11' : 'X11/Xlib.h', 'gl' : 'GL/glx.h', 'egl' : 'EGL/egl.h',
               'xcb' : 'xcb/xinput.h'}
if is_windows
  header_list = {'vulkan' : 'vulkan/vulkan.h', 'gl' : 'GL/glext.h'}
endif
foreach name, header : header_list
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
gl_headers = header_deps['gl']
if is_windows
  x11_headers = []
  egl_headers = []
  win_libs = {}
  foreach l : ['user32', 'gdi32', 'shell32', 'ole32', 'oleaut32', 'advapi32', 'uuid', 'dxguid', 'mmdevapi', 'avrt',
               'ksuser', 'psapi', 'dwmapi', 'shlwapi', 'version']
    win_libs += {l : cpp.find_library(l, required : l not in ['mmdevapi', 'dxguid', 'avrt', 'ksuser'])}
  endforeach
else
  x11_headers = [header_deps['x11'], header_deps['xcb']]
  egl_headers = header_deps['egl']
endif

inc = include_directories('include', 'src')

# Ícone e manifesto (página de código UTF-8, DPI, Windows 10+) nos .exe.
windows_res = []
if is_windows and not layer_only
  windows_res = import('windows').compile_resources('packaging/windows/framux.rc',
    depend_files : files('packaging/windows/framux.manifest', 'assets/icons/framux.ico'))
endif

subdir('src/common')
subdir('src/client')
subdir('src/layer')
# No Windows, jogos D3D/OpenGL são medidos e capturados de fora (ETW e
# Windows Graphics Capture, no daemon); só o Vulkan tem camada no jogo.
if not is_windows
  subdir('src/gl')
endif

if not layer_only
  png_dep = dependency('libpng', required : true)
  jpeg_dep = dependency('libjpeg', required : true)
  if is_windows
    systemd_dep = []
    x11_dep = []
  else
    systemd_dep = dependency('libsystemd', required : true)
    x11_dep = dependency('x11', required : true)
  endif

  subdir('src/core')
  subdir('src/daemon')
  subdir('src/cli')
  subdir('src/gui')
  if is_windows
    subdir('packaging/windows')
  else
    subdir('data')
  endif
  install_headers('include/framux/stats.h', subdir : 'framux')
endif

if get_option('tests') and not is_windows
  subdir('tests')
endif

# `meson devenv -C build` permite rodar `framux run` sem instalar.
devenv = environment()
devenv.set('FRAMUX_GL_PRELOAD', meson.current_build_dir() / 'src/gl/libframux_gl.so')
devenv.prepend('VK_ADD_IMPLICIT_LAYER_PATH', meson.current_build_dir() / 'src/layer')
devenv.set('FRAMUX_DAEMON', meson.current_build_dir() / 'src/daemon/framuxd')
devenv.prepend('PATH', meson.current_build_dir() / 'src/cli', meson.current_build_dir() / 'src/daemon')
meson.add_devenv(devenv)
