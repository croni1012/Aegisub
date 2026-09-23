project('aegisub-scuisei', 'c', version: '0.1.0', license: 'MIT')
python = import('python').find_installation('python3')
system = host_machine.system()
cpu = host_machine.cpu_family()
rust_arch = cpu == 'x86' ? 'i686' : cpu
native_deps = [dependency('threads')]
crt = 'md'
if system == 'windows'
    crt = get_option('b_vscrt')
    assert(meson.get_compiler('c').get_argument_syntax() == 'msvc', 'scuisei currently requires MSVC on Windows')
    rust_target = rust_arch + '-pc-windows-msvc'
    output = 'aegisub_scuisei.lib'
    foreach lib : ['ws2_32', 'userenv', 'bcrypt', 'ntdll', 'advapi32', 'dbghelp']
        native_deps += meson.get_compiler('c').find_library(lib)
    endforeach
elif system == 'darwin'
    rust_target = rust_arch + '-apple-darwin'
    output = 'libaegisub_scuisei.a'
    native_deps += dependency('appleframeworks', modules: ['Security'])
else
    rust_target = rust_arch + '-unknown-linux-gnu'
    output = 'libaegisub_scuisei.a'
    native_deps += meson.get_compiler('c').find_library('dl')
    native_deps += meson.get_compiler('c').find_library('m')
endif
scuisei_lib = custom_target('scuisei detector',
    input: files('aegisub/Cargo.toml'),
    output: output,
    command: [python, files('aegisub/build.py'),
              '@INPUT@', '@OUTPUT@', rust_target, crt],
    depend_files: files('aegisub/Cargo.lock', 'src/aegisub.rs', 'src/detector.rs',
                        'src/error.rs', 'src/postprocess.rs', 'src/simd_metrics.rs', 'src/validation.rs') +
                  files('aegisub/build.py'),
    build_by_default: true,
)
scuisei_dep = declare_dependency(link_with: scuisei_lib, dependencies: native_deps)
