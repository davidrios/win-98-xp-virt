# 2ksbox: the paravirtual Direct3D device (overlay of d3dpt/hw) and
# the d3dpt-vga framebuffer adapter (doc 15), which reuses the VGA core.
i386_system_ss.add(files('d3dpt_mm.c', 'd3dpt_exec_load.c'))
i386_system_ss.add(when: 'CONFIG_VGA', if_true: files('d3dpt_vga.c'))
