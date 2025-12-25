project('animated-tile', 'cpp',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.51.0',
  default_options: [
    'cpp_std=c++17',
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

wayfire = dependency('wayfire')
wfconfig = dependency('wf-config')

add_project_arguments(['-DWLR_USE_UNSTABLE'], language: ['cpp', 'c'])
add_project_arguments(['-DWAYFIRE_PLUGIN'], language: ['cpp', 'c'])
add_project_link_arguments(['-rdynamic'], language:'cpp')

# Allow undefined symbols - resolved when Wayfire loads the plugin
if host_machine.system() != 'darwin'
  add_project_link_arguments(['-Wl,--allow-shlib-undefined'], language:'cpp')
endif

shared_module('animated-tile',
  'src/animated-tile.cpp',
  dependencies: [wayfire, wfconfig],
  install: true,
  install_dir: wayfire.get_variable(pkgconfig: 'plugindir'),
)

# Install metadata
install_data('metadata/animated-tile.xml',
  install_dir: wayfire.get_variable(pkgconfig: 'metadatadir'),
)
