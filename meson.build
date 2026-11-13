project('folksentia', 'cpp',
  version : '0.1.0',
  license : 'AGPL-3.0-or-later',
  default_options : ['cpp_std=c++17', 'buildtype=release', 'warning_level=2'])

cxx = meson.get_compiler('cpp')

# System packages needed to link the static Essentia library. Meson cannot
# install them, so list every missing one at once with the apt command.
# Keep this list in sync with the README.
system_packages = [
  ['eigen3', 'libeigen3-dev'],
  ['fftw3f', 'libfftw3-dev'],
  ['samplerate', 'libsamplerate0-dev'],
  ['libchromaprint', 'libchromaprint-dev'],
  ['libavcodec', 'libavcodec-dev'],
  ['libavformat', 'libavformat-dev'],
  ['libavutil', 'libavutil-dev'],
  ['libswresample', 'libswresample-dev'],
]
missing_packages = []
foreach entry : system_packages
  if not dependency(entry[0], required : false).found()
    missing_packages += entry[1]
  endif
endforeach
if missing_packages.length() > 0
  error('missing system packages: ' + ' '.join(missing_packages) + '\n' +
        'install them with: sudo apt install ' + ' '.join(missing_packages))
endif

# Essentia is built from the submodule by scripts/build-essentia.sh into
# build/essentia-prefix. Override with -Dessentia_prefix=/some/where.
prefix = get_option('essentia_prefix')
if prefix == ''
  prefix = meson.project_source_root() / 'build' / 'essentia-prefix'
endif

libessentia = cxx.find_library('essentia',
  dirs : [prefix / 'lib'],
  static : true,
  required : false)
if not libessentia.found()
  error('static libessentia not found in ' + prefix / 'lib' +
        ' -- run scripts/build-essentia.sh first')
endif

inc = prefix / 'include' / 'essentia'
essentia = declare_dependency(
  dependencies : [
    libessentia,
    # private dependencies of the static library (see --lightweight in
    # scripts/build-essentia.sh)
    dependency('eigen3'),
    dependency('fftw3f'),
    dependency('libavcodec'),
    dependency('libavformat'),
    dependency('libavutil'),
    dependency('libswresample'),
    dependency('samplerate'),
    dependency('libchromaprint'),
    dependency('threads'),
  ],
  compile_args : ['-isystem', prefix / 'include',
                  '-isystem', inc,
                  '-isystem', inc / 'scheduler',
                  '-isystem', inc / 'streaming',
                  '-isystem', inc / 'utils'])

python = find_program('python3')

# Version and build date for --version; regenerated on every build, rewritten
# only when they change.
version_h = vcs_tag(
  input : 'src/version.h.in',
  output : 'version.h',
  # vcs_tag runs the command in the source dir: give it an absolute path.
  command : [python, meson.project_source_root() / 'scripts' / 'version.py',
             meson.project_version(), meson.project_source_root()],
  fallback : '"' + meson.project_version() + '", "unknown"')

exe = executable('folksentia',
  'src/main.cpp', version_h,
  dependencies : essentia,
  install : true)

test('smoke', python, args : [files('tests/smoke.py'), exe], timeout : 600)
