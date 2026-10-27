unit_dir = get_option('systemd_user_unit_dir')
if unit_dir == ''
  unit_dir = prefix / 'lib' / 'systemd' / 'user'
endif

configure_file(input : 'framuxd.service.in',
  output : 'framuxd.service',
  configuration : {'BINDIR' : prefix / get_option('bindir')},
  install : true,
  install_dir : unit_dir)
install_data('framuxd.socket', install_dir : unit_dir)
install_data('config.toml', install_dir : datadir / 'doc' / 'framux', rename : 'config.toml.example')
install_data('io.github.framux.desktop', install_dir : datadir / 'applications')
# Ícone (assets/icons/): vetorial + PNGs por tamanho, com o nome do .desktop.
icons_dir = meson.project_source_root() / 'assets' / 'icons'
install_data(icons_dir / 'framux.svg', rename : 'io.github.framux.svg',
  install_dir : datadir / 'icons' / 'hicolor' / 'scalable' / 'apps')
foreach size : [16, 24, 32, 48, 64, 128, 256, 512]
  install_data(icons_dir / 'png' / 'framux-@0@.png'.format(size), rename : 'io.github.framux.png',
    install_dir : datadir / 'icons' / 'hicolor' / '@0@x@0@'.format(size) / 'apps')
endforeach
