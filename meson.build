project('wirecall', 'c', default_options: [
  'buildtype=release',
  'optimization=3',
  'c_std=gnu23',
  'default_library=static',
  'b_lto=true',
  'b_lto_threads=8',
  'strip=true'
], version: '0.0.1')

include = include_directories('include')
clang_format = find_program('clang-format', required: false)
c = meson.get_compiler('c')

system = host_machine.system()
backend_sources = []
platform_deps = []

if system == 'darwin' or system.endswith('bsd')
  backend_sources += files('src/backend/kqueue.c')
elif system == 'linux'
  backend_sources += files('src/backend/io_uring.c')
elif system == 'windows'
  backend_sources += files('src/backend/windows.c')
  platform_deps += c.find_library('ws2_32', required: true)
else
  error('unsupported Wirecall backend platform: ' + system)
endif

core_sources = files(
  'src/client.c',
  'src/memory.c',
  'src/payload.c',
  'src/protocol.c',
  'src/routes.c',
  'src/scheduler.c',
  'src/server.c',
  'src/trace.c',
)
core_sources += backend_sources

wirecall_core = static_library('wirecall_core',
  core_sources,
  include_directories: include,
  dependencies: platform_deps,
)

wirecall_dep = declare_dependency(
  include_directories: include,
  link_with: wirecall_core,
  dependencies: platform_deps,
)

executable('wirecall-bench', 'demos/bench.c', dependencies: wirecall_dep)
executable('wirecall-embed', 'demos/embed.c', dependencies: wirecall_dep)

executable('wirecall-server', 'demos/server.c', dependencies: wirecall_dep)
executable('wirecall-client', 'demos/client.c', dependencies: wirecall_dep)

executable('wirecall-async-server', 'demos/async/server.c', dependencies: wirecall_dep)
executable('wirecall-async-client', 'demos/async/client.c', dependencies: wirecall_dep)

test_protocol = executable('test_protocol', 'tests/test_protocol.c',
  include_directories: include,
  dependencies: wirecall_dep,
)

test_routes = executable('test_routes', 'tests/test_routes.c',
  include_directories: include,
  link_with: wirecall_core,
)

test_scheduler = executable('test_scheduler', 'tests/test_scheduler.c',
  include_directories: include,
  link_with: wirecall_core,
)

test_integration = executable('test_integration', 'tests/test_integration.c',
  include_directories: include,
  dependencies: wirecall_dep,
)

test('protocol', test_protocol)
test('routes', test_routes)
test('scheduler', test_scheduler)
test('integration', test_integration, timeout: 10)
