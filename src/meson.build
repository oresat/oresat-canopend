subdir('common')
subdir('canopenlinux')
subdir('od_extensions')
subdir('ipc')

libcand_files = [
  'load_configs.c',
  'main.c',
]

libcand_includes = include_directories([
  '.',
])

libcand = shared_library(
  'cand',
  libcand_files,
  include_directories: libcand_includes,
  dependencies: [
    dependency('threads'),
    libcommon_dep,
    libcanopenlinux_dep,
    libodextensions_dep,
    libipc_dep,
  ],
  install: false,
  c_args: [
    '-DCO_MULTIPLE_OD',
    '-DCO_DRIVER_CUSTOM',
    '-DPROJECT_NAME="' + meson.project_name() + '"',
    '-DPROJECT_VERSION="' + meson.project_version() + '"',
  ]
)

libcand_dep = declare_dependency(
  include_directories: libcand_includes,
  link_with: libcand
)
