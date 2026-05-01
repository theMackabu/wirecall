project('rpc', 'c', default_options: [
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

core_sources = files(
  'src/backend/kqueue.c',
  'src/client.c',
  'src/memory.c',
  'src/payload.c',
  'src/protocol.c',
  'src/routes.c',
  'src/scheduler.c',
  'src/server.c',
  'src/trace.c',
)

rpc_core = static_library('rpc_core',
  core_sources,
  include_directories: include,
)

rpc_dep = declare_dependency(
  include_directories: include,
  link_with: rpc_core,
)

executable('rpc-bench', 'demos/bench.c', dependencies: rpc_dep)

executable('rpc-server', 'demos/server.c', dependencies: rpc_dep)
executable('rpc-client', 'demos/client.c', dependencies: rpc_dep)

executable('rpc-async-server', 'demos/async/server.c', dependencies: rpc_dep)
executable('rpc-async-client', 'demos/async/client.c', dependencies: rpc_dep)

test_protocol = executable('test_protocol', 'tests/test_protocol.c',
  include_directories: include,
  dependencies: rpc_dep,
)

test_routes = executable('test_routes', 'tests/test_routes.c',
  include_directories: include,
  link_with: rpc_core,
)

test_scheduler = executable('test_scheduler', 'tests/test_scheduler.c',
  include_directories: include,
  link_with: rpc_core,
)

test_integration = executable('test_integration', 'tests/test_integration.c',
  include_directories: include,
  dependencies: rpc_dep,
)

test('protocol', test_protocol)
test('routes', test_routes)
test('scheduler', test_scheduler)
test('integration', test_integration, timeout: 10)
