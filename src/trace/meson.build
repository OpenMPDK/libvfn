trace_events_c = custom_target('trace events source',
  input: meson.project_source_root() / get_option('trace-events-file'),
  output: 'events.c',
  capture: true,
  command: [perl, trace_pl, '--mode', 'source', '@INPUT@'],
)

trace_sources += trace_events_c

gen_sources += trace_events_c
