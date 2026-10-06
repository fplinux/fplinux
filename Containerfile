# SPDX-License-Identifier: GPL-2.0-only
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG BASE_IMAGE
ARG FPLINUX_IMAGE_RECIPE
ARG FPLINUX_IMAGE_GENERATION
ARG FPLINUX_OFFLINE=0
ARG NODE_VERSION=24.21.0
ARG NODE_SHA256=34ab095af8efe9018f21489d3dc19871fb5edd4f130a71469b69c05fd4e32a5e
ARG NPM_VERSION=12.2.0
ARG NPM_SHA512=66c2632a94e796649738b2d7894d710c2cc2e1696893823066687f6b0d8a52e5f2b54eeaeaa32ffdc513ecca1e49ff794a5c2ec3f9515d879f1b3182b291cf35
ARG TOMLKIT_VERSION=0.15.1
ARG TOMLKIT_SHA256=177a05aece5a8ca5266fd3c448abb47b8d352f09d477d3ca8332db4d89b24304
ARG DTSCHEMA_VERSION=2026.9
ARG DTSCHEMA_SHA256=defcbe1be176dec69c65b9700c9cd992cba3b409976a1404b9c0b5080aa5f4c1
ARG PYTEST_VERSION=9.1.1
ARG PYTEST_SHA256=37a86b45efb9a47a61a36449063e8e18d0cab3161329fc099eb21783169c4f0c
ARG INICONFIG_VERSION=2.3.0
ARG INICONFIG_SHA256=f631c04d2c48c52b84d0d0549c99ff3859c98df65b3101406327ecc7d53fbf12
ARG PLUGGY_VERSION=1.6.0
ARG PLUGGY_SHA256=e920276dd6813095e9377c0bc5566d94c932c33b27a3e3945d8389c374dd4746
ARG PYGMENTS_VERSION=2.21.0
ARG PYGMENTS_SHA256=2363c69b61c4a97c838da3b130dcd6468f4848992b21a82f2a63ec34377137d9
ARG RUFF_VERSION=0.16.10
ARG RUFF_SHA256=ad1b2138407a0c53b936524df99d87211333f7bc40478d6860f42994597f7ff0
ARG SPARSE_COMMIT=37156835e3d725b6d750f000be33ba3814bb2310
ARG SPARSE_SHA256=feca4eb2f0cb61416f4946e0a537d20da8e5eb0d8064fb3f1323a19cb5738ffc
ARG TYPOS_VERSION=1.50.3
ARG TYPOS_SHA256=aca6b5d546307092b8d0a8e0a89dd80f9da51f2f7617c5e45c5607c1684ffbf2
ARG VALE_GOOGLE_VERSION=0.7.0
ARG VALE_GOOGLE_SHA256=a4d6458fef518d51e5e7e84445a066ce73075ca5b8bb71f0feabb344258a4059
ARG LIBTSM_VERSION=4.8.0
ARG LIBTSM_SHA256=50811e9e94fc798eb5dcbaff20f81067c942ec47f329f91bbe4caeb35a61ff05

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    TZ=UTC \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 \
    NPM_CONFIG_AUDIT=false \
    NPM_CONFIG_FUND=false \
    NPM_CONFIG_UPDATE_NOTIFIER=false \
    PATH=/opt/quality/bin:/opt/quality/node/bin:/opt/quality/node-tools/node_modules/.bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

COPY inputs/ /opt/fplinux-inputs/

RUN set -eux; \
    printf '%.32s\n' "${FPLINUX_IMAGE_RECIPE}" > /etc/machine-id; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        apk add --no-network --no-cache /opt/fplinux-inputs/apk/*/x86_64/*.apk; \
    fi; \
    printf '%s\n' \
        'https://dl-cdn.alpinelinux.org/alpine/v3.24/main' \
        'https://dl-cdn.alpinelinux.org/alpine/v3.24/community' \
        > /etc/apk/repositories; \
    if [ "${FPLINUX_OFFLINE}" != 1 ]; then \
        apk add --no-cache \
            7zip=26.01-r0 \
            bash=5.3.9-r1 \
            bzip2=1.0.8-r6 \
            ca-certificates=20260909-r0 \
            ccache=4.13.6-r0 \
            chrpath=0.18-r0 \
            cpio=2.15-r0 \
            curl=8.22.0-r0 \
            diffutils=3.12-r0 \
            dosfstools=4.2-r2 \
            e2fsprogs=1.47.4-r0 \
            e2fsprogs-extra=1.47.4-r0 \
            file=5.47-r2 \
            gawk=5.3.2-r2 \
            genimage=19-r0 \
            git=2.54.0-r0 \
            gzip=1.14-r3 \
            libtool=2.6.0-r1 \
            make=4.4.1-r4 \
            mtools=4.0.49-r0 \
            openssh-client-default=10.3_p1-r1 \
            openssh-server=10.3_p1-r1 \
            patch=2.8-r0 \
            perl=5.42.2-r1 \
            pkgconf=2.5.1-r0 \
            rsync=3.5.0-r0 \
            sed=4.9-r2 \
            squashfs-tools=4.7.5-r0 \
            tar=1.35-r5 \
            unzip=6.0-r16 \
            u-boot-tools=2026.04-r1 \
            wget=1.25.0-r3 \
            which=2.23-r0 \
            xz=5.8.4-r0 \
            zip=3.0-r13 \
            zstd=1.5.7-r2; \
    fi

RUN set -eux; \
    if [ "${FPLINUX_OFFLINE}" != 1 ]; then \
        apk add --no-cache \
            binutils-arm-none-eabi=2.45.1-r0 \
            gcc-arm-none-eabi=16.1.0-r0; \
    fi

RUN set -eux; \
    if [ "${FPLINUX_OFFLINE}" != 1 ]; then \
        apk add --no-cache \
            newlib-arm-none-eabi=4.6.0.20260123-r0; \
    fi

RUN set -eux; \
    if [ "${FPLINUX_OFFLINE}" != 1 ]; then \
        apk add --no-cache \
            abuild=3.17.0-r0 \
            atools-go=0.6.1-r5 \
            build-base=0.5-r4 \
            clang22=22.1.3-r2 \
            linux-headers=7.0.0-r1 \
            lld22=22.1.3-r0 \
            llvm22=22.1.3-r0; \
    fi; \
    adduser -D -u 1000 builder; \
    addgroup builder abuild

RUN set -eux; \
    if [ "${FPLINUX_OFFLINE}" != 1 ]; then \
        apk add --no-cache \
            alsa-lib-dev=1.2.15.3-r0 \
            autoconf=2.73-r0 \
            automake=1.18.1-r1 \
            bc=1.08.2-r1 \
            bison=3.8.2-r3 \
            clang22-analyzer=22.1.3-r2 \
            clang22-extra-tools=22.1.3-r2 \
            dtc=1.7.2-r1 \
            elfutils-dev=0.195-r0 \
            eudev-dev=3.2.14-r6 \
            flex=2.6.4-r8 \
            glib-dev=2.88.1-r1 \
            libffi-dev=3.5.2-r1 \
            libevdev-dev=1.13.6-r0 \
            libusb-dev=1.0.30-r0 \
            ncurses-dev=6.6_p20260516-r0 \
            openssl-dev=3.5.9-r0 \
            swig=4.4.1-r1 \
            xz-dev=5.8.4-r0 \
            zlib-dev=1.3.2-r0; \
    fi

RUN set -eux; \
    if [ "${FPLINUX_OFFLINE}" != 1 ]; then \
        apk add --no-cache \
            py3-dt-schema=2025.12-r1 \
            py3-elftools=0.32-r1 \
            py3-mypy=1.19.1-r2 \
            py3-ruamel.yaml=0.19.1-r0 \
            python3=3.14.8-r0 \
            python3-dev=3.14.8-r0 \
            reuse=6.2.0-r0 \
            shellcheck=0.11.0-r1 \
            shfmt=3.13.1-r2 \
            taplo=0.10.0-r0 \
            vale=3.13.0-r7 \
            yamllint=1.38.0-r0; \
    fi

RUN set -eux; \
    mkdir -p /opt/quality/python /tmp/python-quality; \
    archive=/tmp/python-quality/tomlkit.whl; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/tomlkit/tomlkit-${TOMLKIT_VERSION}-py3-none-any.whl" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://files.pythonhosted.org/packages/13/bc/8c13eb66537dce1d2bd3a57132902f38d0e7f5bb46fa9f4daed9fe9d76ee/tomlkit-${TOMLKIT_VERSION}-py3-none-any.whl"; \
    fi; \
    printf '%s  %s\n' "${TOMLKIT_SHA256}" "${archive}" | sha256sum -c -; \
    python3 -m zipfile -e "${archive}" /opt/quality/python; \
    archive=/tmp/python-quality/dtschema.whl; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/dtschema/dtschema-${DTSCHEMA_VERSION}-py3-none-any.whl" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://files.pythonhosted.org/packages/32/59/e5fafef672bde12c0d237462e5e936650df55e2bfc46aac3164ad5d4e953/dtschema-${DTSCHEMA_VERSION}-py3-none-any.whl"; \
    fi; \
    printf '%s  %s\n' "${DTSCHEMA_SHA256}" "${archive}" | sha256sum -c -; \
    python3 -m zipfile -e "${archive}" /opt/quality/python; \
    archive=/tmp/python-quality/pytest.whl; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/pytest/pytest-${PYTEST_VERSION}-py3-none-any.whl" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://files.pythonhosted.org/packages/24/25/1de2678b631f5a49215c6c96fff41ba892b0a34df68d6d80292b1b48aa7f/pytest-${PYTEST_VERSION}-py3-none-any.whl"; \
    fi; \
    printf '%s  %s\n' "${PYTEST_SHA256}" "${archive}" | sha256sum -c -; \
    python3 -m zipfile -e "${archive}" /opt/quality/python; \
    archive=/tmp/python-quality/iniconfig.whl; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/iniconfig/iniconfig-${INICONFIG_VERSION}-py3-none-any.whl" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://files.pythonhosted.org/packages/cb/b1/3846dd7f199d53cb17f49cba7e651e9ce294d8497c8c150530ed11865bb8/iniconfig-${INICONFIG_VERSION}-py3-none-any.whl"; \
    fi; \
    printf '%s  %s\n' "${INICONFIG_SHA256}" "${archive}" | sha256sum -c -; \
    python3 -m zipfile -e "${archive}" /opt/quality/python; \
    archive=/tmp/python-quality/pluggy.whl; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/pluggy/pluggy-${PLUGGY_VERSION}-py3-none-any.whl" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://files.pythonhosted.org/packages/54/20/4d324d65cc6d9205fabedc306948156824eb9f0ee1633355a8f7ec5c66bf/pluggy-${PLUGGY_VERSION}-py3-none-any.whl"; \
    fi; \
    printf '%s  %s\n' "${PLUGGY_SHA256}" "${archive}" | sha256sum -c -; \
    python3 -m zipfile -e "${archive}" /opt/quality/python; \
    archive=/tmp/python-quality/pygments.whl; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/pygments/pygments-${PYGMENTS_VERSION}-py3-none-any.whl" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://files.pythonhosted.org/packages/71/46/17f022dd3e953bf20a04a028a21ec746d942f8d2af30fa0f124fa0e6a684/pygments-${PYGMENTS_VERSION}-py3-none-any.whl"; \
    fi; \
    printf '%s  %s\n' "${PYGMENTS_SHA256}" "${archive}" | sha256sum -c -; \
    python3 -m zipfile -e "${archive}" /opt/quality/python; \
    python_packages=$(python3 -c 'import sysconfig; print(sysconfig.get_path("purelib"))'); \
    printf '%s\n' "import sys; sys.path.insert(0, '/opt/quality/python')" \
        > "${python_packages}/fplinux-quality.pth"; \
    rm -rf /tmp/python-quality; \
    python3 -B -c 'import tomlkit; import ruamel.yaml; print(tomlkit.__version__); print(ruamel.yaml.__version__)'; \
    python3 -B -c 'import dtschema; import importlib.metadata; print(importlib.metadata.version("dtschema"))'; \
    python3 -B -m pytest --version

RUN set -eux; \
    mkdir -p /opt/quality/node /tmp/node; \
    archive=/tmp/node/node.tar.xz; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/node/node-v${NODE_VERSION}-linux-x64-musl.tar.xz" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64-musl.tar.xz"; \
    fi; \
    printf '%s  %s\n' "${NODE_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xJf "${archive}" -C /opt/quality/node --strip-components=1; \
    rm -rf /opt/quality/node/lib/node_modules/npm; \
    mkdir -p /opt/quality/node/lib/node_modules/npm; \
    archive=/tmp/node/npm.tar.gz; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/npm/npm-${NPM_VERSION}.tgz" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://registry.npmjs.org/npm/-/npm-${NPM_VERSION}.tgz"; \
    fi; \
    printf '%s  %s\n' "${NPM_SHA512}" "${archive}" | sha512sum -c -; \
    tar -xzf "${archive}" -C /opt/quality/node/lib/node_modules/npm --strip-components=1; \
    ln -sf ../lib/node_modules/npm/bin/npm-cli.js /opt/quality/node/bin/npm; \
    ln -sf ../lib/node_modules/npm/bin/npx-cli.js /opt/quality/node/bin/npx; \
    rm -rf /tmp/node; \
    test "$(node --version)" = "v${NODE_VERSION}"; \
    test "$(npm --version)" = "${NPM_VERSION}"

RUN set -eux; \
    mkdir -p /opt/quality/bin /tmp/quality; \
    archive=/tmp/quality/sparse.tar.gz; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/sparse/sparse-${SPARSE_COMMIT}.tar.gz" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://git.kernel.org/pub/scm/devel/sparse/sparse.git/snapshot/sparse-${SPARSE_COMMIT}.tar.gz"; \
    fi; \
    printf '%s  %s\n' "${SPARSE_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xzf "${archive}" -C /tmp/quality; \
    make -C "/tmp/quality/sparse-${SPARSE_COMMIT}" -j2 sparse; \
    install -m 0755 "/tmp/quality/sparse-${SPARSE_COMMIT}/sparse" /opt/quality/bin/sparse; \
    rm -rf /tmp/quality; \
    sparse --version

RUN set -eux; \
    mkdir -p /usr/local/bin; \
    ln -s /usr/bin/clang /usr/local/bin/armv7-alpine-linux-musleabihf-cc; \
    ln -s /usr/bin/clang++ /usr/local/bin/armv7-alpine-linux-musleabihf-c++; \
    ln -s /usr/bin/ld.lld /usr/local/bin/armv7-alpine-linux-musleabihf-ld; \
    for tool in ar nm objcopy objdump ranlib readelf size strings strip; do \
        ln -s "/usr/bin/arm-none-eabi-${tool}" "/usr/local/bin/armv7-alpine-linux-musleabihf-${tool}"; \
    done; \
    armv7-alpine-linux-musleabihf-cc -dumpmachine | grep -qx armv7-alpine-linux-musleabihf; \
    arm-none-eabi-gcc --version | head -n 1

RUN set -eux; \
    mkdir -p /opt/quality/bin /tmp/quality; \
    archive=/tmp/quality/ruff.tar.gz; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/ruff/ruff-x86_64-unknown-linux-musl.tar.gz" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://github.com/astral-sh/ruff/releases/download/${RUFF_VERSION}/ruff-x86_64-unknown-linux-musl.tar.gz"; \
    fi; \
    printf '%s  %s\n' "${RUFF_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xzf "${archive}" -C /tmp/quality; \
    install -m 0755 "/tmp/quality/ruff-x86_64-unknown-linux-musl/ruff" /opt/quality/bin/ruff; \
    rm -rf /tmp/quality; \
    ruff --version; \
    reuse --version

WORKDIR /workspace

COPY package.json package-lock.json /opt/quality/node-tools/
RUN set -eux; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        for archive in /opt/fplinux-inputs/npm/*.tgz; do \
            npm cache add --offline --ignore-scripts "${archive}"; \
        done; \
        npm ci --offline --ignore-scripts --prefix /opt/quality/node-tools; \
    else \
        npm ci --ignore-scripts --prefix /opt/quality/node-tools; \
    fi; \
    commitlint --version; \
    prettier --version; \
    markdownlint-cli2 --version

RUN set -eux; \
    mkdir -p /opt/quality/bin /opt/quality/vale-styles/Google /tmp/quality; \
    archive=/tmp/quality/typos.tar.gz; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/typos/typos-v${TYPOS_VERSION}-x86_64-unknown-linux-musl.tar.gz" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://github.com/crate-ci/typos/releases/download/v${TYPOS_VERSION}/typos-v${TYPOS_VERSION}-x86_64-unknown-linux-musl.tar.gz"; \
    fi; \
    printf '%s  %s\n' "${TYPOS_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xzf "${archive}" -C /opt/quality/bin ./typos; \
    archive=/tmp/quality/vale-google.tar.gz; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/vale-google/v${VALE_GOOGLE_VERSION}.tar.gz" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://github.com/vale-cli/Google/archive/refs/tags/v${VALE_GOOGLE_VERSION}.tar.gz"; \
    fi; \
    printf '%s  %s\n' "${VALE_GOOGLE_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xzf "${archive}" \
        -C /opt/quality/vale-styles/Google \
        --strip-components=2 \
        "Google-${VALE_GOOGLE_VERSION}/Google"; \
    rm -rf /tmp/quality; \
    typos --version; \
    vale --version

ARG GITLEAKS_VERSION=8.30.1
ARG GITLEAKS_SHA256=551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb
RUN set -eux; \
    mkdir -p /tmp/scanners; \
    archive=/tmp/scanners/gitleaks.tar.gz; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/gitleaks/gitleaks_${GITLEAKS_VERSION}_linux_x64.tar.gz" "${archive}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${archive}" \
            "https://github.com/gitleaks/gitleaks/releases/download/v${GITLEAKS_VERSION}/gitleaks_${GITLEAKS_VERSION}_linux_x64.tar.gz"; \
    fi; \
    printf '%s  %s\n' "${GITLEAKS_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xzf "${archive}" -C /tmp/scanners gitleaks; \
    install -m 0755 /tmp/scanners/gitleaks /opt/quality/bin/gitleaks; \
    rm -rf /tmp/scanners; \
    mkdir -p /tmp/gitleaks-selftest; \
    printf 'token = "ghp_%s%s"\n' '7cE9vKwP3qN8sXbL5rM2tDfA' '6zH4uYjR1gV0' \
        > /tmp/gitleaks-selftest/secret.txt; \
    if gitleaks detect --no-banner --no-git --source /tmp/gitleaks-selftest --exit-code 1; then exit 1; \
    else test "$?" -eq 1; fi; \
    rm -rf /tmp/gitleaks-selftest; \
    gitleaks version

ARG HADOLINT_VERSION=2.15.1
ARG HADOLINT_SHA256=c7187db94eeeeca956519a6af171adc31453941a1e777961f6e680f697c8c507
RUN set -eux; \
    binary=/tmp/hadolint; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/hadolint/hadolint-linux-x86_64" "${binary}"; \
    else \
        curl -fsSL --retry 3 \
            --output "${binary}" \
            "https://github.com/hadolint/hadolint/releases/download/v${HADOLINT_VERSION}/hadolint-linux-x86_64"; \
    fi; \
    printf '%s  %s\n' "${HADOLINT_SHA256}" "${binary}" | sha256sum -c -; \
    install -m 0755 "${binary}" /opt/quality/bin/hadolint; \
    rm -f "${binary}"; \
    hadolint --version

RUN set -eux; \
    if [ "${FPLINUX_OFFLINE}" != 1 ]; then \
        apk add --no-cache \
            cmake=4.2.3-r0 \
            dbus=1.16.2-r2 \
            dbus-dev=1.16.2-r2 \
            libdrm-dev=2.4.134-r0 \
            libinput-dev=1.31.3-r0 \
            libjpeg-turbo-dev=3.1.3-r0 \
            libxkbcommon-dev=1.13.1-r0 \
            meson=1.11.1-r0 \
            samurai=1.2-r8; \
    fi

COPY alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch /tmp/libtsm/function-keys.patch
RUN set -eux; \
    mkdir -p /tmp/libtsm; \
    if [ "${FPLINUX_OFFLINE}" = 1 ]; then \
        cp "/opt/fplinux-inputs/sources/libtsm/v${LIBTSM_VERSION}.tar.gz" /tmp/libtsm/source.tar.gz; \
    else \
        curl -fsSL --retry 3 \
            --output /tmp/libtsm/source.tar.gz \
            "https://github.com/kmscon/libtsm/archive/refs/tags/v${LIBTSM_VERSION}.tar.gz"; \
    fi; \
    printf '%s  %s\n' "${LIBTSM_SHA256}" /tmp/libtsm/source.tar.gz | sha256sum -c -; \
    tar -xzf /tmp/libtsm/source.tar.gz -C /tmp/libtsm; \
    tsm_source="/tmp/libtsm/libtsm-${LIBTSM_VERSION}"; \
    patch -d "${tsm_source}" -p1 < /tmp/libtsm/function-keys.patch; \
    cc -Os -std=gnu99 -D_GNU_SOURCE -fPIC -shared \
        -Wl,-soname,libtsm.so.4 -Wl,--version-script="${tsm_source}/src/tsm/libtsm.sym" \
        -I"${tsm_source}/src/tsm" -I"${tsm_source}/src/shared" \
        -I"${tsm_source}/external" -I"${tsm_source}/external/wcwidth" \
        "${tsm_source}/src/tsm/tsm-render.c" "${tsm_source}/src/tsm/tsm-screen.c" \
        "${tsm_source}/src/tsm/tsm-selection.c" "${tsm_source}/src/tsm/tsm-unicode.c" \
        "${tsm_source}/src/tsm/tsm-vte-charsets.c" "${tsm_source}/src/tsm/tsm-vte.c" \
        "${tsm_source}/src/shared/shl-htable.c" "${tsm_source}/external/wcwidth/wcwidth.c" \
        -o /tmp/libtsm/libtsm.so.4; \
    install -m 0755 /tmp/libtsm/libtsm.so.4 /usr/lib/libtsm.so.4; \
    ln -s libtsm.so.4 /usr/lib/libtsm.so; \
    install -m 0644 "${tsm_source}/src/tsm/libtsm.h" /usr/include/libtsm.h; \
    { printf '%s\n' 'prefix=/usr' 'libdir=/usr/lib' 'includedir=/usr/include' \
        'Name: libtsm' 'Description: Terminal state machine'; \
      printf 'Version: %s\n' "${LIBTSM_VERSION}"; \
      printf '%s\n' 'Libs: -L/usr/lib -ltsm' 'Cflags: -I/usr/include'; } \
        > /usr/lib/pkgconfig/libtsm.pc; \
    rm -rf /tmp/libtsm; \
    pkg-config --modversion libtsm

RUN rm -rf /opt/fplinux-inputs /root/.npm /var/log/apk.log

RUN mkdir -p /cache/analysis /cache/ccache /cache/downloads /cache/host-tools \
    /cache/linux /cache/rootfs \
    /tmp/fplinux-home /workspace /work \
    && chmod 1777 /cache /tmp/fplinux-home /work

COPY scripts/fplinux_cli/environment/image_content.py /usr/local/libexec/fplinux-image-content.py

RUN set -eu; \
    content=$(python3 -B /usr/local/libexec/fplinux-image-content.py); \
    printf '%s\n%s\n%s\n' "${FPLINUX_IMAGE_RECIPE}" "${FPLINUX_IMAGE_GENERATION}" "${content}" \
        > /etc/fplinux-image-state

LABEL org.opencontainers.image.title="FPLinux build environment" \
      org.opencontainers.image.description="Reproducible Alpine Linux/amd64 environment for FPLinux kernel, APK and RAM-image builds" \
      org.opencontainers.image.base.name="${BASE_IMAGE}" \
      org.opencontainers.image.licenses="GPL-2.0-only"

WORKDIR /workspace
CMD ["/bin/bash"]
