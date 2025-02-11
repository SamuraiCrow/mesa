#!/bin/bash
# shellcheck disable=SC2086 # we want word splitting

set -ex

git config --global user.email "mesa@example.com"
git config --global user.name "Mesa CI"
git clone \
    https://github.com/helen-fornazier/VK-GL-CTS.git \
    -b vulkan-cts-1.3.3.1_mesa-android-2023-01-19 \
    --depth 1 \
    /VK-GL-CTS
pushd /VK-GL-CTS

# Apply a patch to update zlib link to an available version.
# vulkan-cts-1.3.3.0 uses zlib 1.2.12 which was removed from zlib server due to
# a CVE. See https://zlib.net/
# FIXME: Remove this patch when uprev to 1.3.4.0+
curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
    "https://github.com/KhronosGroup/VK-GL-CTS/commit/6bb2e7d64261bedb503947b1b251b1eeeb49be73.patch" | git am -

# Apply a patch to fix a bug in 1.3.3.0 that affects some new formats
curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
    "https://github.com/KhronosGroup/VK-GL-CTS/commit/4fa2b40411921b304f5dad8d106b212ad5b0f172.patch" | git am -

# https://github.com/KhronosGroup/VK-GL-CTS/pull/360
sed -i -e 's#http://zlib.net/zlib-1.2.12.tar.gz#http://zlib.net/fossils/zlib-1.2.12.tar.gz#g' external/fetch_sources.py

# --insecure is due to SSL cert failures hitting sourceforge for zlib and
# libpng (sigh).  The archives get their checksums checked anyway, and git
# always goes through ssh or https.
python3 external/fetch_sources.py --insecure

mkdir -p /deqp

# Save the testlog stylesheets:
cp doc/testlog-stylesheet/testlog.{css,xsl} /deqp
popd

pushd /deqp

if [ "${DEQP_TARGET}" != 'android' ]; then
    # When including EGL/X11 testing, do that build first and save off its
    # deqp-egl binary.
    cmake -S /VK-GL-CTS -B . -G Ninja \
        -DDEQP_TARGET=x11_egl_glx \
        -DCMAKE_BUILD_TYPE=Release \
        $EXTRA_CMAKE_ARGS
    ninja modules/egl/deqp-egl
    cp /deqp/modules/egl/deqp-egl /deqp/modules/egl/deqp-egl-x11

    cmake -S /VK-GL-CTS -B . -G Ninja \
        -DDEQP_TARGET=wayland \
        -DCMAKE_BUILD_TYPE=Release \
        $EXTRA_CMAKE_ARGS
    ninja modules/egl/deqp-egl
    cp /deqp/modules/egl/deqp-egl /deqp/modules/egl/deqp-egl-wayland
fi

cmake -S /VK-GL-CTS -B . -G Ninja \
      -DDEQP_TARGET=${DEQP_TARGET:-x11_glx} \
      -DCMAKE_BUILD_TYPE=Release \
      $EXTRA_CMAKE_ARGS
ninja

if [ "${DEQP_TARGET}" != 'android' ]; then
    mv /deqp/modules/egl/deqp-egl-x11 /deqp/modules/egl/deqp-egl
fi

# Copy out the mustpass lists we want.
mkdir /deqp/mustpass
for mustpass in $(< /VK-GL-CTS/external/vulkancts/mustpass/main/vk-default.txt) ; do
    cat /VK-GL-CTS/external/vulkancts/mustpass/main/$mustpass \
        >> /deqp/mustpass/vk-master.txt
done

if [ "${DEQP_TARGET}" != 'android' ]; then
    cp \
        /deqp/external/openglcts/modules/gl_cts/data/mustpass/gles/aosp_mustpass/3.2.6.x/*.txt \
        /deqp/mustpass/.
    cp \
        /deqp/external/openglcts/modules/gl_cts/data/mustpass/egl/aosp_mustpass/3.2.6.x/egl-master.txt \
        /deqp/mustpass/.
    cp \
        /deqp/external/openglcts/modules/gl_cts/data/mustpass/gles/khronos_mustpass/3.2.6.x/*-master.txt \
        /deqp/mustpass/.
    cp \
        /deqp/external/openglcts/modules/gl_cts/data/mustpass/gl/khronos_mustpass/4.6.1.x/*-master.txt \
        /deqp/mustpass/.
    cp \
        /deqp/external/openglcts/modules/gl_cts/data/mustpass/gl/khronos_mustpass_single/4.6.1.x/*-single.txt \
        /deqp/mustpass/.

    # Save *some* executor utils, but otherwise strip things down
    # to reduct deqp build size:
    mkdir /deqp/executor.save
    cp /deqp/executor/testlog-to-* /deqp/executor.save
    rm -rf /deqp/executor
    mv /deqp/executor.save /deqp/executor
fi

# Remove other mustpass files, since we saved off the ones we wanted to conventient locations above.
rm -rf /deqp/external/openglcts/modules/gl_cts/data/mustpass
rm -rf /deqp/external/vulkancts/modules/vulkan/vk-master*
rm -rf /deqp/external/vulkancts/modules/vulkan/vk-default

rm -rf /deqp/external/openglcts/modules/cts-runner
rm -rf /deqp/modules/internal
rm -rf /deqp/execserver
rm -rf /deqp/framework
# shellcheck disable=SC2038,SC2185 # TODO: rewrite find
find -iname '*cmake*' -o -name '*ninja*' -o -name '*.o' -o -name '*.a' | xargs rm -rf
${STRIP_CMD:-strip} external/vulkancts/modules/vulkan/deqp-vk
${STRIP_CMD:-strip} external/openglcts/modules/glcts
${STRIP_CMD:-strip} modules/*/deqp-*
du -sh ./*
rm -rf /VK-GL-CTS
popd
