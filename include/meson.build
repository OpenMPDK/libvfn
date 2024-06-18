vfn_inc_strs=['.']
kernel_header_path = get_option('kernel-header-path')
if kernel_header_path != ''
  vfn_inc_strs += [kernel_header_path]
endif

vfn_inc = include_directories(vfn_inc_strs)

subdir('vfn')
