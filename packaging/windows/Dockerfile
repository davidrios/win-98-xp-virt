# The Windows cross-build image (docs/build-windows.md, M11).
#
# Everything that produces a Windows artefact is built in here, so a
# Windows package is reproducible from any Linux box and nobody has to
# grow a mingw sysroot by hand: Arch (this project's Linux host) has
# mingw-w64-gcc but no mingw glib2/pixman/epoxy, which is exactly what
# QEMU needs. Fedora packages all of them, and QEMU's own Windows CI
# uses the same base (qemu/tests/docker/dockerfiles/fedora-win64-cross.docker)
# -- so a QEMU build failure here is one upstream would see too.
#
# What it adds over QEMU's image: rustup with the x86_64-pc-windows-gnu
# target (the player, the launcher and libdisc are Rust and must match
# the mingw ABI of the QEMU DLL they link) and the packaging tools.
#
#   scripts/win-cross.sh --build        # build/refresh this image
#   scripts/win-cross.sh <command...>   # run a command in it, in the checkout
ARG FEDORA=43
FROM registry.fedoraproject.org/fedora:${FEDORA}

# Native side: what configure, meson and the generators run on.
RUN dnf install -y --setopt=install_weak_deps=False \
        bash bison bzip2 ca-certificates diffutils file findutils flex gcc gcc-c++ \
        git-core make meson ninja-build patch pkgconf \
        python3 python3-pip python3-setuptools rsync sed tar which xorriso zip zstd \
    && dnf clean all -y

# QEMU 9.2's mkvenv supports Python 3.8-3.13 and Fedora's default is newer
# (3.14 breaks it), so the build gets its own interpreter -- the same
# constraint scripts/configure-qemu.sh enforces on every host, here met by
# a package rather than by uv. `distlib` is installed for real because
# QEMU's fallback to pip's vendored copy is broken on pip >= 26
# (CLAUDE.md's "found no usable distlib"), and this image has no network
# during a build.
RUN dnf install -y --setopt=install_weak_deps=False python3.13 \
    && python3.13 -m ensurepip \
    && python3.13 -m pip install --no-cache-dir distlib setuptools \
    && dnf clean all -y
ENV QEMU_PYTHON=/usr/bin/python3.13

# Cross side: the mingw64 sysroot QEMU and the executor compile against.
# libepoxy is the Mesa pass-through's GL loader; SDL2 is qemu-3dfx's
# native UI dependency; gettext/iconv arrive as glib's own dependencies
# but are named here because the packaging step copies their DLLs.
RUN dnf install -y --setopt=install_weak_deps=False \
        mingw-w64-tools \
        mingw64-gcc mingw64-gcc-c++ mingw64-headers mingw64-winpthreads \
        mingw64-glib2 mingw64-pixman mingw64-zlib mingw64-libepoxy \
        mingw64-SDL2 mingw64-gettext mingw64-win-iconv mingw64-pkg-config \
    && dnf clean all -y

# Rust, in the image rather than on the host: the same stable channel the
# native build uses, plus the windows-gnu target. CARGO_HOME/RUSTUP_HOME
# live under /opt so a bind-mounted checkout never inherits (or writes)
# the host user's cargo state.
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain stable \
          --component rustfmt --component clippy \
          --target x86_64-unknown-linux-gnu --target x86_64-pc-windows-gnu \
    && chmod -R a+rwX /opt/rustup /opt/cargo

# The linker rustc calls for x86_64-pc-windows-gnu, and the pkg-config
# that answers for the mingw sysroot rather than for Fedora itself.
ENV CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc \
    PKG_CONFIG=/usr/bin/x86_64-w64-mingw32-pkg-config \
    LANG=C.UTF-8
