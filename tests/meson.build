with_xvfb = find_program('with_xvfb.sh')
x11_link = dependency('x11', required : false)
xcb_link = dependency('xcb', required : false)
gl_link = dependency('gl', required : false)
vulkan_link = dependency('vulkan', required : false)

# Apps de teste próprios: existem também em 32 bits (não há vkcube i686).
test_env = {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'empty-config',
            'XDG_RUNTIME_DIR' : meson.current_build_dir() / 'no-runtime'}
if vulkan_link.found() and xcb_link.found()
  vk_present = executable('vk_present_test', files('vk_present_test.cpp'), dependencies : [vulkan_link, xcb_link])
  test('layer-validation', find_program('vk_validate.sh'),
    args : [vk_present, meson.project_build_root() / 'src' / 'layer'],
    env : test_env,
    depends : framux_vk,
    timeout : 60)
endif
if gl_link.found() and x11_link.found()
  gl_state = executable('gl_state_test', files('gl_state_test.cpp'), dependencies : [gl_link, x11_link])
  test('gl-state', with_xvfb,
    args : [gl_state],
    env : test_env + {'LD_PRELOAD' : framux_gl.full_path()},
    depends : framux_gl,
    timeout : 60)
  # Com uma biblioteca que intercepta dlopen como o overlay da Steam.
  fake_dlopen_hook = shared_library('fake_dlopen_hook', files('fake_dlopen_hook.c'), dependencies : dl_dep)
  test('gl-state-dlopen-hook', with_xvfb,
    args : [gl_state],
    env : test_env + {'LD_PRELOAD' : framux_gl.full_path() + ':' + fake_dlopen_hook.full_path()},
    depends : [framux_gl, fake_dlopen_hook],
    timeout : 60)
endif

if layer_only
  subdir_done()
endif

catch2_dep = dependency('catch2', required : false)
if not catch2_dep.found()
  catch2_dep = dependency('catch2-with-main', required : false)
endif

if catch2_dep.found()
  unit = executable('framux_tests',
    files('test_main.cpp', 'test_arbiter.cpp', 'test_bench.cpp', 'test_common.cpp', 'test_flatpak.cpp',
          'test_fps_counter.cpp', 'test_image.cpp'),
    include_directories : include_directories('../src'),
    dependencies : [framux_core_dep, catch2_dep, x11_headers])
  test('unit', unit, timeout : 120)
else
  warning('Catch2 não encontrado: testes unitários desativados')
endif

png_probe = executable('png_probe', files('png_probe.cpp'), dependencies : png_dep)
e2e_deps = [framux_gl, framux_vk, framuxd, framux_cli, png_probe]
xtst_dep = dependency('xtst', required : false)
e2e_scenarios = ['vulkan', 'gl', 'kill', 'reconnect', 'pause', 'auto', 'reload']
if xtst_dep.found()
  e2e_deps += executable('xkey', files('xkey.cpp'), dependencies : [xtst_dep, x11_dep])
  e2e_scenarios += 'hotkeys'
endif


e2e = find_program('e2e.sh')
foreach scenario : e2e_scenarios
  test('e2e-' + scenario, e2e,
    args : [meson.project_build_root(), scenario],
    depends : e2e_deps,
    is_parallel : false,
    timeout : 120)
endforeach

if is_variable('framux_gui_dep')
  qt6_test_dep = dependency('qt6', modules : ['Test'], required : false)
  if qt6_test_dep.found()
    gui_test = executable('gui_test', files('gui_test.cpp'), dependencies : [framux_gui_dep, qt6_test_dep])
    test('gui', gui_test, env : {'QT_QPA_PLATFORM' : 'offscreen'}, timeout : 60)
  endif
endif
