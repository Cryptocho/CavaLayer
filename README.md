Cavalayer - Audio Visualizer for Wayland

A lightweight audio visualizer client for Wayland compositors supporting the layer-shell protocol, inspired by https://github.com/fsobolev/cavalier. This implementation uses cava as the backend for audio processing and renders the visualization using OpenGL through EGL.

Features

• 🎵 Real-time audio visualization using cava backend

• 🖥️ Wayland native with layer-shell protocol support

• 🔵 Smooth cardinal spline rendering with OpenGL

• ⌨️ Basic keyboard interactivity (ESC to exit)

• 🏗️ Modular architecture with clean resource management

Planned Features

• 🌈 Gradient color support

• ✨ Bloom/glow effects

• 🔄 Circular visualization mode

• ⚙️ GUI configuration interface

• 🎨 Customizable color schemes

Requirements

• Wayland compositor with layer-shell support (Sway, Hyprland, etc.)

• cava installed and available in PATH

• EGL and OpenGL libraries


Configuration

Currently configuration is hardcoded in the source. Future versions will support:

• Config file (~/.config/cavalayer/config)

• Runtime arguments

• GUI configuration interface


License

MIT License
