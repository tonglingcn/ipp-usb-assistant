#!/usr/bin/env bash
# IPP-USB 免驱助手 构建脚本 (UOS 20, DTK5 + Qt5)
#
# 针对 UOS 20 Professional（Debian 10 系，Qt 5.11.3 + DTK5 5.6.x）适配：
#   - 构建依赖检测改为 Qt5 / Dtk5 对应的 -dev 包；
#   - 运行时依赖与 Deepin 25 版一致（ipp-usb / cups / sane-airscan / avahi 等）；
#   - CMakeLists.txt 已内置 Qt5/DTK5 双兼容探测，无需额外参数即可直接编出 Qt5 版本；
#   - 打包使用本目录下的 debian-uos/ 元数据（Build-Depends/Depends 均为 Qt5/DTK5）。
#
# 用法:
#   ./build-uos.sh            # 仅编译
#   ./build-uos.sh --deb      # 编译并打包 deb
#   ./build-uos.sh --clean    # 清理构建目录
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
APP_NAME="ipp-usb-assistant"

case "${1:-}" in
    --clean)
        echo "==> 清理构建目录 ..."
        rm -rf "$BUILD_DIR"
        echo "    已清理: $BUILD_DIR"
        exit 0
        ;;
esac

echo "==> 检查构建依赖 ..."
MISSING=""
for pkg in cmake build-essential pkg-config qtbase5-dev qttools5-dev \
           libdtkcore-dev libdtkgui-dev libdtkwidget-dev \
           libcups2-dev libsane-dev; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        MISSING="$MISSING $pkg"
    fi
done

if [ -n "$MISSING" ]; then
    echo "==> 安装缺失的构建依赖 (需要 root 密码) ..."
    sudo apt-get update -y
    # shellcheck disable=SC2086
    sudo apt-get install -y $MISSING
else
    echo "    构建依赖已齐全，跳过安装。"
fi

# 运行时底层服务（外设适配核心）。不安装也能编译，但程序无法正常工作。
if ! dpkg -s ipp-usb >/dev/null 2>&1 || ! dpkg -s sane-airscan >/dev/null 2>&1; then
    echo "==> 安装运行时依赖 (需要 root 密码) ..."
    sudo apt-get install -y \
        ipp-usb \
        cups \
        cups-filters \
        sane \
        sane-airscan \
        avahi-daemon
else
    echo "    运行时依赖已齐全，跳过安装。"
fi

echo "==> 配置与构建 ..."
cd "$PROJECT_DIR"
cmake -S . -B "$BUILD_DIR" -DCMAKE_INSTALL_PREFIX=/usr

# 先单独跑完 AUTOMOC 代码生成（moc_*.cpp），再整体并行编译，
# 避免 clean 后首次构建时 mocs_compilation.cpp.o 抢在最后一个
# moc 文件写出之前开始编译而报 "No such file" 的偶发竞态。
cmake --build "$BUILD_DIR" --target ipp-usb-assistant_autogen -j"$(nproc)"
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "==> 构建完成："
echo "    可执行文件: $BUILD_DIR/$APP_NAME"

# ---- deb 打包 ----
if [ "${1:-}" = "--deb" ]; then
    echo ""
    echo "==> 打包 deb (UOS 20 / Qt5 / DTK5) ..."

    # debhelper 需要干净的源码树：构建产物、编辑器备份等不能进包。
    # 这里复制一份到临时目录，并使用 debian-uos/ 作为打包元数据。
    PKG_TMP="$(mktemp -d /tmp/ipp-usb-assistant-uos-pkg-XXXXXX)"
    trap 'rm -rf "$PKG_TMP"' EXIT

    # 排除 debian/：那是 deepin 25 / UOS 25 的 Qt6 元数据，
    # 这里要用 debian-uos/ 顶替，先不复制以免出现两套 debian/
    tar -cf - --exclude=build --exclude='*.zip' --exclude=debian . \
        | (cd "$PKG_TMP" && tar -xf -)

    # 用 UOS 版元数据作为 debian/（Qt5/DTK5 的 Build-Depends/Depends）
    cp -r "$PROJECT_DIR/debian-uos" "$PKG_TMP/debian"

    # 清理上次打包可能残留的中间文件
    rm -f "$PKG_TMP"/debian/files "$PKG_TMP"/debian/*.substvars
    rm -rf "$PKG_TMP"/debian/.debhelper "$PKG_TMP"/debian/ipp-usb-assistant

    (cd "$PKG_TMP" && dpkg-buildpackage -us -uc -b)

    DEB=$(ls -t /tmp/${APP_NAME}_*.deb 2>/dev/null | head -1)
    if [ -n "$DEB" ]; then
        echo "    deb 包: $DEB"
        echo ""
        echo "安装: sudo dpkg -i $DEB"
        echo "卸载: sudo dpkg -r $APP_NAME"
    else
        echo "    [错误] 未找到生成的 deb 包"
        exit 1
    fi
fi

echo ""
echo "运行（建议将当前用户加入 lp/scanner 组以具备设备权限）:"
echo "    $BUILD_DIR/$APP_NAME"
