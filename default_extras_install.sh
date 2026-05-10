#!/usr/bin/env sh

# Remove old Gamescope default configs and add our own.
mkdir -p "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope"
rm -rf "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/scripts" || true
rm -rf "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/looks" || true
cp -r "${MESON_SOURCE_ROOT}/scripts" "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/scripts"
cp -r "${MESON_SOURCE_ROOT}/looks" "${DESTDIR}/${MESON_INSTALL_PREFIX}/share/gamescope/looks"
