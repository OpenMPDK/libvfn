device_test_sources = [
  files('common.c'),

  # custom (generated) targets
  ccan_config_h,
  trace_events_h,
]

device_tests = {
  'aer': ['aer.c'],
  'flush': ['flush.c'],
  'identify': ['identify.c'],
  'io': ['io.c'],
  'open': ['open.c'],
}

foreach device_test, sources : device_tests
  exe = executable(device_test, [device_test_sources, sources],
    dependencies: [libnvme],
    link_with: [ccan_lib, vfn_lib],
    include_directories: [ccan_inc, vfn_inc],
  )

  test(device_test, exe, suite: ['device'], is_parallel: false)

endforeach
