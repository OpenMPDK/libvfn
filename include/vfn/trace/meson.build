trace_events_h = custom_target('events.h',
  input: meson.project_source_root() / get_option('trace-events-file'),
  output: 'events.h',
  capture: true,
  command: [perl, trace_pl, '--mode', 'header', '@INPUT@'],
  install: true,
  install_dir: get_option('includedir') / 'vfn/trace',
)
