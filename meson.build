project(
  'localed',
  'c',
  version         : '0.3.0',
  license         : 'GPL-2.0-or-later',
  meson_version   : '>= 0.58.0',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'werror=false',
    'optimization=2',
  ],
)

cc = meson.get_compiler('c')

extra_c_args = cc.get_supported_arguments([
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-fstack-protector-strong',
])

# Enable fortified libc functions only when optimization is active.
optimized_src = '''
  #if !defined(__OPTIMIZE__) || __OPTIMIZE__ == 0
  #error Optimization is disabled
  #endif
'''

if cc.compiles(optimized_src, name : 'compiler optimization enabled')
  extra_c_args += cc.get_supported_arguments([
    '-D_FORTIFY_SOURCE=2',
  ])
endif

if get_option('buildtype').contains('debug')
  extra_c_args += cc.get_supported_arguments([
    '-Wcast-align',
    '-Wmissing-declarations',
    '-Wmissing-prototypes',
    '-Wnested-externs',
  ])
endif

add_project_arguments(extra_c_args, language : 'c')

prefix     = get_option('prefix')
sysconfdir = get_option('sysconfdir')
datadir    = get_option('datadir')
localedir  = join_paths(prefix, get_option('localedir'))

if not sysconfdir.startswith('/')
  sysconfdir = join_paths(prefix, sysconfdir)
endif

glib_req   = '>= 2.56'

glib_dep = dependency('glib-2.0',         version : glib_req)
gio_dep  = dependency('gio-2.0',          version : glib_req)
gio_unix = dependency('gio-unix-2.0',     version : glib_req)
# Used by data/meson.build to discover the PolicyKit action installation
# directory. The daemon talks to PolicyKit over its managed GDBus connection
# and does not link against polkit-gobject-1.
polkit = dependency('polkit-gobject-1', version : '>= 0.113')

i18n = import('i18n')

gettext_package = meson.project_name()
# Make gettext macros available in all C translation units, including tests.
# GETTEXT_PACKAGE is defined by the project compiler arguments below.
add_project_arguments('-include', 'glib/gi18n-lib.h', language : 'c')
add_project_arguments(
  '-DGETTEXT_PACKAGE="@0@"'.format(gettext_package),
  language : 'c',
)

conf = configuration_data()
conf.set_quoted('PACKAGE',         meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('VERSION',         meson.project_version())
conf.set_quoted('GETTEXT_PACKAGE', gettext_package)
conf.set_quoted('LOCALEDIR',       localedir)
conf.set_quoted('SYSCONFDIR',      sysconfdir)

# Distro-specific tunables (meson_options.txt).
# PRIVILEGED_GROUP: group considered "admin" by polkit rules.
# lang_sh:          path to the Slackware locale shell script
#   (/etc/profile.d/lang.sh; sourced at login via /etc/profile;
#    uses "export KEY=value" syntax, not the KEY=value format of
#    /etc/locale.conf).  localed reads and writes this file directly.
# rc_keymap:        path to Slackware's virtual console keyboard script.
# x11_keyboard_conf: path to the Xorg input class keyboard config file.
conf.set_quoted('PRIVILEGED_GROUP',     get_option('privileged-group'))
conf.set_quoted('RCL_LANG_SH_FILE',     get_option('lang_sh'))
conf.set_quoted('RCL_RC_KEYMAP_FILE',    get_option('rc_keymap'))
conf.set_quoted('RCL_RC_KEYMAP_TOGGLE_FILE',
                get_option('rc_keymap') + '.toggle')
conf.set_quoted('RCL_X11_KEYBOARD_FILE', get_option('x11_keyboard_conf'))

desktop_backend = get_option('desktop-backend')
conf.set10('HAVE_GNOME_BACKEND', desktop_backend == 'gnome')
conf.set10('HAVE_PLASMA6_BACKEND', desktop_backend == 'plasma6')

input_sources_helper_dir = get_option('libexecdir')
if not input_sources_helper_dir.startswith('/')
  input_sources_helper_dir = join_paths(prefix, input_sources_helper_dir)
endif
conf.set_quoted('RCL_INPUT_SOURCES_HELPER',
                join_paths(input_sources_helper_dir, 'localed-input-sources-helper'))

conf.set('GLIB_VERSION_MIN_REQUIRED', 'GLIB_VERSION_2_56')
conf.set('GLIB_VERSION_MAX_ALLOWED',  'GLIB_VERSION_2_56')

configure_file(
  output        : 'config.h',
  configuration : conf,
)

config_h_inc = include_directories('.')

subdir('src')
if get_option('tests')
  subdir('tests')
endif
subdir('data')
# Install the Slackware localectl client for normal Meson installs too.
install_data(
  'slack/localectl',
  install_dir : get_option('bindir'),
  install_mode : 'rwxr-xr-x',
)

subdir('po')
subdir('man')

summary({
  'locale script' : get_option('lang_sh'),
  'console keymap': get_option('rc_keymap'),
  'X11 keyboard'  : get_option('x11_keyboard_conf'),
  'privileged group': get_option('privileged-group'),
  'desktop backend'  : desktop_backend,
}, section : 'Configuration')
