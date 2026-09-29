mp_test_sources = files('test_memory_protector.c')
if host_system == 'windows'
  mp_test_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_memory_protector',
    '--FILEDESC', 'test_memory_protector - memory pressure policy tests'])
endif
mp_test = shared_module('test_memory_protector', mp_test_sources,
  kwargs: pg_test_mod_args)
test_install_libs += mp_test
test_install_data += files('test_memory_protector.control',
  'test_memory_protector--1.0.sql')
tests += {
  'name': 'test_memory_protector',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': ['test_memory_protector'],
    'regress_args': ['--temp-config', files('observe.conf')],
  },
}
