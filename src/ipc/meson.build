libipc_files = [
  'ipc.c',
  'ipc_broadcast.c',
  'ipc_consume.c',
  'ipc_respond.c',
]

libipc_includes = include_directories('.')

libipc = shared_library(
  'ipc',
  libipc_files,
  include_directories: libipc_includes,
  dependencies: [
    dependency('libzmq'),
    libcommon_dep,
    libcanopenlinux_dep,
    libodextensions_dep,
  ],
  install: false,
  c_args: [
    '-DCO_MULTIPLE_OD',
    '-DCO_DRIVER_CUSTOM',
  ],
)

libipc_dep = declare_dependency(
  include_directories: libipc_includes,
  link_with: libipc
)
