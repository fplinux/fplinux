# SPDX-License-Identifier: GPL-2.0-only
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG BASE_IMAGE
ARG RUFF_VERSION=0.16.0
ARG RUFF_SHA256=2138b7bc58ff877f5bba09aea4cc984ad5699433b6a3f811003527b8cff8e9ad
ARG PYTHON311_VERSION=3.11.16
ARG PYTHON311_SHA256=91bcdebfdde239a003ae93738a7fce0f9230fee5c4bc2b86f6e6e8c6f98aabe8
ARG SPARSE_COMMIT=37156835e3d725b6d750f000be33ba3814bb2310
ARG SPARSE_SHA256=feca4eb2f0cb61416f4946e0a537d20da8e5eb0d8064fb3f1323a19cb5738ffc
ARG TYPOS_VERSION=1.48.0
ARG TYPOS_SHA256=72a930c9a94fc3914aa56835c5b859c892a797d40c1c42638b98d93f16ff519c
ARG VALE_GOOGLE_VERSION=0.7.0
ARG VALE_GOOGLE_SHA256=a4d6458fef518d51e5e7e84445a066ce73075ca5b8bb71f0feabb344258a4059

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    TZ=UTC \
    NPM_CONFIG_AUDIT=false \
    NPM_CONFIG_FUND=false \
    NPM_CONFIG_UPDATE_NOTIFIER=false \
    PATH=/opt/quality/bin:/opt/quality/node-tools/node_modules/.bin:${PATH}

RUN set -eux; \
    printf '%s\n' \
        'https://dl-cdn.alpinelinux.org/alpine/v3.24/main' \
        'https://dl-cdn.alpinelinux.org/alpine/v3.24/community' \
        > /etc/apk/repositories; \
    apk add --no-cache \
        7zip=26.01-r0 \
        bash=5.3.9-r1 \
        bzip2=1.0.8-r6 \
        ca-certificates=20260611-r0 \
        cpio=2.15-r0 \
        curl=8.21.0-r0 \
        diffutils=3.12-r0 \
        file=5.47-r2 \
        gawk=5.3.2-r2 \
        git=2.54.0-r0 \
        gzip=1.14-r2 \
        libtool=2.6.0-r1 \
        make=4.4.1-r4 \
        openssh-client-default=10.3_p1-r0 \
        patch=2.8-r0 \
        perl=5.42.2-r0 \
        pkgconf=2.5.1-r0 \
        rsync=3.4.3-r1 \
        sed=4.9-r2 \
        tar=1.35-r5 \
        unzip=6.0-r16 \
        wget=1.25.0-r3 \
        which=2.23-r0 \
        xz=5.8.3-r0 \
        zip=3.0-r13 \
        zstd=1.5.7-r2

RUN set -eux; \
    apk add --no-cache \
        binutils-arm-none-eabi=2.45.1-r0 \
        gcc-arm-none-eabi=16.1.0-r0

RUN set -eux; \
    apk add --no-cache \
        newlib-arm-none-eabi=4.6.0.20260123-r0

RUN set -eux; \
    apk add --no-cache \
        abuild=3.17.0-r0 \
        atools-go=0.6.1-r4 \
        build-base=0.5-r4 \
        clang22=22.1.3-r2 \
        linux-headers=7.0.0-r1 \
        lld22=22.1.3-r0; \
    adduser -D -u 1000 builder; \
    addgroup builder abuild

RUN set -eux; \
    apk add --no-cache \
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
        libffi-dev=3.5.2-r1 \
        libusb-dev=1.0.30-r0 \
        ncurses-dev=6.6_p20260516-r0 \
        openssl-dev=3.5.8-r0 \
        swig=4.4.1-r1 \
        xz-dev=5.8.3-r0 \
        zlib-dev=1.3.2-r0

RUN set -eux; \
    apk add --no-cache \
        nodejs=24.18.1-r0 \
        npm=11.12.1-r0 \
        py3-dt-schema=2025.12-r1 \
        py3-elftools=0.32-r1 \
        py3-mypy=1.19.1-r2 \
        python3=3.14.7-r1 \
        python3-dev=3.14.7-r1 \
        reuse=6.2.0-r0 \
        shellcheck=0.11.0-r1 \
        shfmt=3.13.1-r1 \
        taplo=0.10.0-r0 \
        vale=3.13.0-r6

RUN set -eux; \
    mkdir -p /opt/quality/bin /tmp/quality; \
    archive=/tmp/quality/sparse.tar.gz; \
    curl -fsSL --retry 3 \
        --output "${archive}" \
        "https://git.kernel.org/pub/scm/devel/sparse/sparse.git/snapshot/sparse-${SPARSE_COMMIT}.tar.gz"; \
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
    curl -fsSL --retry 3 \
        --output "${archive}" \
        "https://github.com/astral-sh/ruff/releases/download/${RUFF_VERSION}/ruff-x86_64-unknown-linux-musl.tar.gz"; \
    printf '%s  %s\n' "${RUFF_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xzf "${archive}" -C /tmp/quality; \
    install -m 0755 "/tmp/quality/ruff-x86_64-unknown-linux-musl/ruff" /opt/quality/bin/ruff; \
    rm -rf /tmp/quality; \
    ruff --version; \
    reuse --version

WORKDIR /tmp/python/Python-${PYTHON311_VERSION}
RUN set -eux; \
    mkdir -p /tmp/python; \
    archive=/tmp/python/Python-${PYTHON311_VERSION}.tar.xz; \
    curl -fsSL --retry 3 \
        --output "${archive}" \
        "https://www.python.org/ftp/python/${PYTHON311_VERSION}/Python-${PYTHON311_VERSION}.tar.xz"; \
    printf '%s  %s\n' "${PYTHON311_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xJf "${archive}" -C /tmp/python; \
    ./configure --prefix=/opt/quality/python311 --without-ensurepip; \
    make -j2; \
    make altinstall; \
    ln -s /opt/quality/python311/bin/python3.11 /opt/quality/bin/python3.11; \
    rm -rf /tmp/python; \
    python3.11 --version; \
    python3.11 -c 'import ctypes, lzma, tomllib, zlib'

WORKDIR /workspace

COPY package.json package-lock.json /opt/quality/node-tools/
RUN set -eux; \
    npm ci --ignore-scripts --prefix /opt/quality/node-tools; \
    prettier --version; \
    markdownlint-cli2 --version

RUN set -eux; \
    mkdir -p /opt/quality/bin /opt/quality/vale-styles/Google /tmp/quality; \
    archive=/tmp/quality/typos.tar.gz; \
    curl -fsSL --retry 3 \
        --output "${archive}" \
        "https://github.com/crate-ci/typos/releases/download/v${TYPOS_VERSION}/typos-v${TYPOS_VERSION}-x86_64-unknown-linux-musl.tar.gz"; \
    printf '%s  %s\n' "${TYPOS_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xzf "${archive}" -C /opt/quality/bin ./typos; \
    archive=/tmp/quality/vale-google.tar.gz; \
    curl -fsSL --retry 3 \
        --output "${archive}" \
        "https://github.com/vale-cli/Google/archive/refs/tags/v${VALE_GOOGLE_VERSION}.tar.gz"; \
    printf '%s  %s\n' "${VALE_GOOGLE_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xzf "${archive}" \
        -C /opt/quality/vale-styles/Google \
        --strip-components=2 \
        "Google-${VALE_GOOGLE_VERSION}/Google"; \
    rm -rf /tmp/quality; \
    typos --version; \
    vale --version

ARG GITLEAKS_VERSION=8.18.4
ARG GITLEAKS_SHA256=ba6dbb656933921c775ee5a2d1c13a91046e7952e9d919f9bac4cec61d628e7d
RUN set -eux; \
    mkdir -p /tmp/scanners; \
    archive=/tmp/scanners/gitleaks.tar.gz; \
    curl -fsSL --retry 3 \
        --output "${archive}" \
        "https://github.com/gitleaks/gitleaks/releases/download/v${GITLEAKS_VERSION}/gitleaks_${GITLEAKS_VERSION}_linux_x64.tar.gz"; \
    printf '%s  %s\n' "${GITLEAKS_SHA256}" "${archive}" | sha256sum -c -; \
    tar -xzf "${archive}" -C /tmp/scanners gitleaks; \
    install -m 0755 /tmp/scanners/gitleaks /opt/quality/bin/gitleaks; \
    rm -rf /tmp/scanners; \
    mkdir -p /tmp/gitleaks-selftest; \
    printf 'token = "ghp_%s%s"\n' 'aBcDeFgHiJkLmNoPqRsTuVwXyZ' '0123456789' \
        > /tmp/gitleaks-selftest/secret.txt; \
    if gitleaks detect --no-banner --no-git --source /tmp/gitleaks-selftest --exit-code 1; then exit 1; \
    else test "$?" -eq 1; fi; \
    rm -rf /tmp/gitleaks-selftest; \
    gitleaks version

ARG HADOLINT_VERSION=2.15.1
ARG HADOLINT_SHA256=c7187db94eeeeca956519a6af171adc31453941a1e777961f6e680f697c8c507
RUN set -eux; \
    binary=/tmp/hadolint; \
    curl -fsSL --retry 3 \
        --output "${binary}" \
        "https://github.com/hadolint/hadolint/releases/download/v${HADOLINT_VERSION}/hadolint-linux-x86_64"; \
    printf '%s  %s\n' "${HADOLINT_SHA256}" "${binary}" | sha256sum -c -; \
    install -m 0755 "${binary}" /opt/quality/bin/hadolint; \
    rm -f "${binary}"; \
    hadolint --version

RUN mkdir -p /cache/analysis /cache/downloads /cache/linux /cache/rootfs \
    /tmp/fplinux-home /workspace /work \
    && chmod 1777 /cache /tmp/fplinux-home /work

LABEL org.opencontainers.image.title="FPLinux build environment" \
      org.opencontainers.image.description="Reproducible Alpine Linux/amd64 environment for FPLinux kernel, APK and RAM-image builds" \
      org.opencontainers.image.base.name="${BASE_IMAGE}" \
      org.opencontainers.image.licenses="GPL-2.0-only"

WORKDIR /workspace
CMD ["/bin/bash"]
