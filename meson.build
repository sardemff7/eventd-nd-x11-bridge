project('eventd-nd-x11-bridge', 'c',
    version: '0',
    meson_version: '>=0.39.1',
    license: [ 'GPL3+' ],
    default_options: [
        'c_std=gnu11',
        'warning_level=2',
    ],
)


headers = [
    'stdlib.h',
    'stdio.h',
    'errno.h',
    'signal.h',
    'string.h',
    'sys/mman.h',
]
c_compiler = meson.get_compiler('c')
foreach h : headers
    if not c_compiler.has_header(h)
        error('Header @0@ was not found, but is required'.format(h))
    endif
endforeach

glib_min_major='2'
glib_min_minor='40'
glib_min_version='.'.join([glib_min_major, glib_min_minor])
wayland_min_version='1.12.92'
weston_supported_majors = [
    '5',
    '4',
]
last_weston_major = weston_supported_majors[weston_supported_majors.length() - 1]
weston_min_version='1.12.90'


librt = c_compiler.find_library('rt', required: false)
glib = dependency('glib-2.0', version: '>= @0@'.format(glib_min_version))
gmodule = dependency('gmodule-2.0')
wayland_server = dependency('wayland-server', version: '>= @0@'.format(wayland_min_version))

weston_major = ''
foreach try_weston_major : weston_supported_majors
    if weston_major == ''
        libweston = dependency('libweston-@0@'.format(try_weston_major), required: last_weston_major == try_weston_major)
        libweston_desktop = dependency('libweston-desktop-@0@'.format(try_weston_major), required: last_weston_major == try_weston_major)
        if libweston.found()
            weston_major = try_weston_major
        endif
    endif
endforeach
weston = dependency('weston')
libgwater_wayland_server = subproject('libgwater/wayland-server').get_variable('libgwater_wayland_server')

xcb = [
    subproject('libgwater/xcb').get_variable('libgwater_xcb'),
    dependency('xcb'),
    dependency('xcb-aux'),
    dependency('xcb-shm'),
    dependency('xcb-randr'),
    dependency('xcb-xfixes'),
    dependency('xcb-ewmh'),
    dependency('xcb-xkb'),
    dependency('xkbcommon'),
    dependency('xkbcommon-x11'),
    dependency('cairo-xcb'),
]

header_conf = configuration_data()
header_conf.set_quoted('PACKAGE_NAME', meson.project_name())
header_conf.set_quoted('PACKAGE_VERSION', meson.project_version())
header_conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())

header_conf.set('EVENTD_EXPORT', '__attribute__((visibility("default")))')

header_conf.set_quoted('WESTON_PLUGINS_DIR', join_paths(weston.get_pkgconfig_variable('libdir'), 'weston'))
header_conf.set_quoted('LIBWESTON_PLUGINS_DIR', join_paths(libweston.get_pkgconfig_variable('libdir'), 'libweston-@0@'.format(weston_major)))

header_conf.set_quoted('BUILD_DIR', meson.current_build_dir())

config_h = configure_file(output: 'config.h', configuration: header_conf)

add_project_arguments(
    '-fvisibility=hidden',
    language: 'c'
)
flags = [
    '-Wformat=2',
    '-Wno-unused-parameter',
]
foreach f : flags
    if c_compiler.has_argument(f)
        add_project_arguments(f, language: 'c')
    endif
endforeach

executable('eventd-nd-x11-bridge', files('main.c'),
    dependencies: [
        libweston,
        wayland_server,
        libgwater_wayland_server,
        gmodule,
        glib,
    ]
)

shared_module('eventd-nd-x11-bridge', files('backend.c'),
    dependencies: [
        xcb,
        librt,
        libweston,
        wayland_server,
        glib,
    ],
    name_prefix: '',
)
