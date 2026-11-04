project('folksentia', 'cpp',
  version : '0.1.0',
  license : 'AGPL-3.0-or-later',
  default_options : ['cpp_std=c++17', 'buildtype=release', 'warning_level=2'])

cxx = meson.get_compiler('cpp')

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
    dependency('threads'),
  ],
  compile_args : ['-isystem', prefix / 'include',
                  '-isystem', inc,
                  '-isystem', inc / 'scheduler',
                  '-isystem', inc / 'streaming',
                  '-isystem', inc / 'utils'])

executable('folksentia',
  'src/main.cpp',
  dependencies : essentia,
  install : true)
