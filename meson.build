project('wayland-window', 'c')

sources = files(
    'source/main.c',
    'source/utils.c',
    'source/extensions/xdg-shell-protocol.c',
)

dependencies = [
    dependency('wayland-client'),
    dependency('wayland-cursor'),
    dependency('xkbcommon'),
]

executable('wayland-window', sources, dependencies: dependencies, install: true)
