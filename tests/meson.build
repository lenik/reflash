massage_test_exe = executable(
    'massage_test',
    [
        'massage_test.cpp',
        '../src/options.cpp',
        '../src/engine/progress.cpp',
        '../src/engine/pause.cpp',
        '../src/engine/massage.cpp',
        '../src/engine/sha1_pass.cpp',
        '../src/io/device.cpp',
        '../src/io/partition.cpp',
        '../src/io/block_io.cpp',
        '../src/db/store.cpp',
        '../src/mount/automount.cpp',
        '../src/fs/detect.cpp',
        '../src/fs/fat.cpp',
        '../src/fs/exfat.cpp',
        '../src/fs/ntfs.cpp',
        '../src/fs/ext.cpp',
    ],
    include_directories: include_directories('../src', '..'),
    dependencies: [sqlite_dep, crypto_dep],
    install: false,
)

test('massage', massage_test_exe, timeout: 120)
